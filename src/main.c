/*
 * eink_central v3 — 墨水屏图片显示（主机）
 *
 * 链路：扫描→连接→MTU协商→发现→订阅→唤醒→会话参数→文件信息→数据分片×N→传输完成→结束确认
 *
 * 图片数据：image_data.h 由取模工具生成（296x128 1bit 点阵，image_packets[21][233]）
 * 板子：官方 nRF52840 DK（主机角色）
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/printk.h>
#include "image_data.h"   /* 取模工具生成的点阵数据（image_packets[21][233]）*/
#include "compressed_rev.h"      /* type1: 反转层（红层，张梁取反） */
#include "compressed_image.h"   /* type2: 正层（黑层，张梁点阵） */

/* ===== 墨水屏信息 ===== */
#define EINK_MAC_STR "AA:BB:CC:DD:EE:FF"  // 目标墨水屏标签地址，请替换为你自己的设备
static bt_addr_le_t eink_addr;

/* NUS 写入通道：49535343-8841-43f4-a8d4-ecbe34729bb3（倒序） */
static struct bt_uuid_128 uuid_write = BT_UUID_INIT_128(
	0xb3, 0x9b, 0x72, 0x34, 0xbe, 0xec, 0xd4, 0xa8,
	0xf4, 0x43, 0x41, 0x88, 0x43, 0x53, 0x53, 0x49);
/* NUS 通知通道：49535343-1e4d-4bd9-ba61-23c647249616（倒序） */
static struct bt_uuid_128 uuid_notify = BT_UUID_INIT_128(
	0x16, 0x96, 0x24, 0x47, 0xc6, 0x23, 0x61, 0xba,
	0xd9, 0x4b, 0x4d, 0x1e, 0x43, 0x53, 0x53, 0x49);

/* ===== 连接状态 ===== */
static struct bt_conn *eink_conn;
static uint16_t write_handle;
static uint16_t notify_handle;
static uint16_t ccc_handle;
static uint8_t frame_buf[256];     /* 0x82/0x83 发送帧缓冲 */
static uint8_t frame_buf2[256];    /* 0x85 发送帧缓冲 */
static uint8_t frame84_buf[256];   /* 0x84 发送帧专用缓冲（异步发送期间不可被覆盖） */
static bool file_done;             /* 文件传输已完成标志（0x05 后置位） */
/* ===== QuickLZ 压缩数据（PC 端生成） ===== */
static const uint8_t *qlz_out = compressed_rev;   /* type1: 反转层（红层） */   
static size_t qlz_len = COMPRESSED_REV_LEN;   /* type1 反转层 */
static uint16_t qlz_packets = COMPRESSED_IMAGE_PACKETS;   /* 0x83 包数（启动时按块边界重算） */

/* ===== QuickLZ 块边界切包表（0x84 按完整块发，避免切断块头） ===== */
#define MAX_PKT 16
static uint16_t pkt_off[MAX_PKT];
static uint16_t pkt_len[MAX_PKT];
static uint16_t pkt_cnt;

static void build_pkt_table(const uint8_t *data, size_t len)
{
	size_t off = 0;
	pkt_cnt = 0;
	while (off < len && pkt_cnt < MAX_PKT) {
		uint16_t pstart = off;
		while (off + 4 <= len) {
			uint32_t blk = ((uint32_t)data[off] << 24) |
				       ((uint32_t)data[off + 1] << 16) |
				       ((uint32_t)data[off + 2] << 8) | data[off + 3];
			if (blk == 0 || off + 4 + blk > len)
				break;
			if (off + 4 + blk - pstart > 233)
				break;   /* 再装一个块会超 233B，封包 */
			off += 4 + blk;
		}
		if (off == pstart)
			break;   /* 防死循环 */
		pkt_off[pkt_cnt] = pstart;
		pkt_len[pkt_cnt] = off - pstart;
		pkt_cnt++;
	}
}


static const uint8_t *qlz_out2 = compressed_image;    /* type2: 正层（黑层） */
static size_t qlz_len2 = COMPRESSED_IMAGE_LEN;   /* type2 正层 */
static uint16_t qlz_packets2 = (COMPRESSED_IMAGE_LEN + 232) / 233;   /* type2 正层 */

static struct bt_le_conn_param conn_param = {
	.interval_min = BT_GAP_INIT_CONN_INT_MIN,
	.interval_max = BT_GAP_INIT_CONN_INT_MAX,
	.latency = 0,
	.timeout = 400,
};

/* 唤醒包：标签动作=0x0A（APP 有图片文件需要更新） */
static const uint8_t wake_frame[] = {
	0x00, 0x0F, 0x01, 0x01, 0x69, 0x26, 0x55, 0x0D, 0x0A,
	0x00, 0x00, 0x00, 0x00, 0x26, 0x9E
};

/* ============================================================
 * CRC16：poly=0xA001, init=0xAAAA, refin=true
 * ============================================================ */
static uint16_t crc16_calc(const uint8_t *data, size_t len)
{
	uint16_t a = 0xAAAA;
	for (size_t i = 0; i < len; i++) {
		a ^= data[i];
		for (int j = 0; j < 8; j++) {
			if (a & 1)
				a = (a >> 1) ^ 0xA001;
			else
				a >>= 1;
		}
	}
	return a;
}

/* ============================================================
 * 帧打包：长度(2B大端) + 包序(1B) + 命令(1B) + 数据(NB) + CRC(2B大端)
 * ============================================================ */
static int pack_frame(uint8_t *out, uint8_t seq, uint8_t cmd,
		      const uint8_t *data, size_t data_len)
{
	uint16_t total = 2 + 1 + 1 + data_len + 2;
	out[0] = (total >> 8) & 0xFF;
	out[1] = total & 0xFF;
	out[2] = seq;
	out[3] = cmd;
	if (data)
		memcpy(&out[4], data, data_len);

	uint16_t crc = crc16_calc(out, 4 + data_len);
	out[4 + data_len] = (crc >> 8) & 0xFF;          /* CRC 高字节在前 */
	out[4 + data_len + 1] = crc & 0xFF;
	return total;
}

/* ===== 写回调/写参数（提前声明） ===== */
static void write_func(struct bt_conn *conn, uint8_t err,
		       struct bt_gatt_write_params *params);
static struct bt_gatt_write_params write_params;

/* 发送保护标志：0x84 大帧写进行中时，0x02/0x03 回复先跳过 */
static uint8_t tx_active;

/* 发送一帧到设备（一次写完，MTU=247 时 242B 帧单包发出） */
static void send_frame(const uint8_t *buf, int len)
{
	write_params.data   = buf;
	write_params.length = len;
	write_params.func   = write_func;
	write_params.handle = write_handle;
	write_params.offset = 0;
	bt_gatt_write(eink_conn, &write_params);
}

/* ============================================================
 * 完整帧处理：设备问什么，我们答什么
 * ============================================================ */
static void handle_frame(const uint8_t *data, uint16_t len)
{
	uint8_t cmd = data[3];
	uint8_t seq = data[2];

	/* 唤醒确认 0x81 */
	if (cmd == 0x81) {
		printk("Wake confirmed (0x81). Waiting for device status...\n");
		return;
	}

	/* 设备上报状态 0x02 → 回 0x82（标签动作=0x0A 有文件更新） */
	if (cmd == 0x02) {
		if (tx_active) return;   /* 0x84 写进行中，不抢发送通道 */
		printk("Device reports status (0x02). Reply 0x82...\n");
		/* 标签动作：传输完成后回 0=通讯结束（终止循环），否则 10=有文件更新 */
		uint8_t action = file_done ? 0x00 : 0x0A;
		uint8_t reply_data[17] = {
			0x69, 0x26, 0x55, 0x0D,        /* 系统时间 */
			action,                          /* 标签动作 */
			0x00, 0x00, 0x00, 0x01,        /* 设备id = 1（导师配方） */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* PIN */
			0x00, 0x00,                    /* 文件ID */
		};
		int n = pack_frame(frame_buf, seq, 0x82,
				   reply_data, sizeof(reply_data));
		send_frame(frame_buf, n);
		return;
	}

	/* 设备问文件信息 0x03 → 回 0x83
	 * 0x03 请求 = 设备ID(4B) + 文件类型(1B) + 传输包大小(2B) + 文件ID(2B) + 数据类型(1B)
	 * 0x83 回复 = 文件大小(4B) + 文件类型(1B) + 包数量(2B) + 自动刷新(4B) + 周期刷新(4B) + 屏幕显示(1B)
	 * 文件类型回显设备请求（1=背景图 2=前景图） */
	if (cmd == 0x03) {
		if (tx_active) return;   /* 0x84 写进行中，不抢发送通道 */
		printk("Device asks file info (0x03). Reply 0x83...\n");
		uint8_t req_filetype = data[8];
		/* 导师配方：文件大小 = 真实压缩大小，包数 = 1 */
		size_t f_len = (req_filetype == 1) ? qlz_len : qlz_len2;
		uint16_t f_pkts = (req_filetype == 1) ? qlz_packets : qlz_packets2;

		uint8_t info_data[16] = {
			(uint8_t)((f_len >> 24) & 0xFF),
			(uint8_t)((f_len >> 16) & 0xFF),
			(uint8_t)((f_len >> 8) & 0xFF),
			(uint8_t)(f_len & 0xFF),        /* 文件大小 */
			0x01,                           /* 文件类型（下面覆盖为回显值） */
			(uint8_t)((f_pkts >> 8) & 0xFF),
			(uint8_t)(f_pkts & 0xFF),       /* 包数量 */
			0x00, 0x00, 0x00, 0x00,        /* 自动刷新时间 = 0 */
			0x00, 0x00, 0x00, 0x00,        /* 周期刷新时间 = 0 */
			0x01,                          /* 屏幕显示 = 1（屏幕1显示） */
		};
		info_data[4] = req_filetype;   /* 回显设备请求的文件类型 */
		info_data[15] = 0x03;  /* 屏幕显示 = 3（双屏相同，导师配方） */
		int n = pack_frame(frame_buf, seq, 0x83,
				   info_data, sizeof(info_data));
		send_frame(frame_buf, n);
		return;
	}

	/* 设备要数据包 0x04 → 回 0x84
	 * 0x04 请求 = 设备ID(4B) + 文件ID(2B) + 包位置(2B) + 文件类型(1B)
	 * 0x84 回复 = 文件类型(1B) + 包位置(2B) + 图片数据(NB) */
	if (cmd == 0x04) {
		uint16_t req_pos  = (data[10] << 8) | data[11];
		uint8_t req_type  = data[12];
		printk("Device asks data (0x04, seq=0x%02X, pos=%u, type=%u). Reply 0x84...\n",
		       seq, req_pos, req_type);

		static uint8_t pkt[236];
		pkt[0] = req_type;
		pkt[1] = (req_pos >> 8) & 0xFF;
		pkt[2] = req_pos & 0xFF;
		uint16_t img_len = 233;
		if (req_type == 1) {
			/* type1: 按 QuickLZ 块边界切包（每包含整数个完整块） */
			uint32_t off = (uint32_t)req_pos * 233;
			if (off < qlz_len) {
				img_len = (qlz_len - off) < 233 ? (uint16_t)(qlz_len - off) : 233;
				memcpy(&pkt[3], &qlz_out[off], img_len);
				printk("[0x84] type1 pos=%u, off=%u, len=%u\n", req_pos, off, img_len);
			} else {
				img_len = 0;
			}
		} else {
			uint32_t off = (uint32_t)req_pos * 233;
			if (off < qlz_len2) {
				img_len = (qlz_len2 - off) < 233 ? (uint16_t)(qlz_len2 - off) : 233;
				memcpy(&pkt[3], &qlz_out2[off], img_len);
			} else {
				img_len = 0;
			}
		}

		int n = pack_frame(frame84_buf, seq, 0x84, pkt, 3 + img_len);
		send_frame(frame84_buf, n);   /* 一次写完 */
		return;
	}

	/* 设备报告传输完成 0x05 → 回 0x85 → 发 0x82 动作=1 通知刷屏 */
	if (cmd == 0x05) {
		uint16_t f_id = (data[8] << 8) | data[9];   /* 文件ID */
		printk("Transfer complete (0x05)! FileID=%u\n", f_id);
		file_done = true;                  /* 标记传输完成 */

		/* 回 0x85：结束标志 = 0（收到）——导师配方：之后直接断开，无刷屏指令 */
		static const uint8_t done_data[] = { 0x00 };
		int n = pack_frame(frame_buf2, seq, 0x85, done_data, sizeof(done_data));
		send_frame(frame_buf2, n);
		return;
	}

	printk("Unknown cmd 0x%02X (seq=0x%02X)\n", cmd, seq);
}

/* ============================================================
 * NUS 字节流粘包处理：累积数据，按长度字段切帧
 * ============================================================ */
static uint8_t rx_buf[512];
static uint16_t rx_len;

static void rx_append(const uint8_t *data, uint16_t len)
{
	if (rx_len + len > sizeof(rx_buf)) {
		printk("RX buffer overflow!\n");
		rx_len = 0;
		return;
	}
	memcpy(&rx_buf[rx_len], data, len);
	rx_len += len;

	/* 循环切帧：长度字段 = 整帧字节数 */
	while (rx_len >= 2) {
		uint16_t frame_len = (rx_buf[0] << 8) | rx_buf[1];
		if (frame_len < 4 || frame_len > sizeof(rx_buf)) {
			printk("Bad frame len %u, reset buffer\n", frame_len);
			rx_len = 0;
			return;
		}
		if (rx_len < frame_len)
			break;   /* 数据还不够一帧，等更多 */

		handle_frame(rx_buf, frame_len);
		memmove(rx_buf, &rx_buf[frame_len], rx_len - frame_len);
		rx_len -= frame_len;
	}
}

/* ===== 通知回调：数据交给粘包缓冲 ===== */
static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	if (data) {
		printk("[NOTIFY] %u bytes\n", length);
		rx_append((const uint8_t *)data, length);
	}
	return BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_subscribe_params subscribe_params = {
	.notify = notify_func,
	.value = BT_GATT_CCC_NOTIFY,
};

/* ===== 写回调 ===== */
static void write_func(struct bt_conn *conn, uint8_t err,
		       struct bt_gatt_write_params *params)
{
	if (err) {
		printk("Write FAILED (err %d) [MTU=%u]\n", err, bt_gatt_get_mtu(conn));
		return;
	}
	printk("Write OK [MTU=%u]\n", bt_gatt_get_mtu(conn));
}

/* ===== 订阅完成回调 → 发唤醒包 ===== */
static void subscribe_cb(struct bt_conn *conn, uint8_t err,
			 struct bt_gatt_subscribe_params *params)
{
	if (err) {
		printk("Subscribe FAILED (%d)\n", err);
		return;
	}
	printk("Subscribe OK! Sending wake frame...\n");

	write_params.func   = write_func;
	write_params.handle = write_handle;
	write_params.offset = 0;
	write_params.data   = wake_frame;
	write_params.length = sizeof(wake_frame);
	bt_gatt_write(conn, &write_params);
}

/* ===== discover 回调 ===== */
static struct bt_gatt_discover_params discover_params;

static uint8_t discover_func(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  struct bt_gatt_discover_params *params)
{
	if (!attr) {
		if (write_handle == 0)
			printk("Write char NOT found!\n");
		else if (notify_handle == 0)
			printk("Notify char NOT found!\n");
		return BT_GATT_ITER_STOP;
	}
	if (write_handle == 0) {
		write_handle = bt_gatt_attr_value_handle(attr);
		printk("Write char handle=%u\n", write_handle);
		discover_params.uuid = &uuid_notify.uuid;
		discover_params.start_handle = 1;
		discover_params.end_handle = 0xFFFF;
		bt_gatt_discover(conn, &discover_params);
		return BT_GATT_ITER_STOP;
	} else if (notify_handle == 0) {
		notify_handle = bt_gatt_attr_value_handle(attr);
		printk("Notify char handle=%u\n", notify_handle);
		ccc_handle = notify_handle + 1;
		printk("CCCD handle=%u (auto)\n", ccc_handle);
		subscribe_params.value_handle = notify_handle;
		subscribe_params.ccc_handle = ccc_handle;
		subscribe_params.subscribe = subscribe_cb;
		bt_gatt_subscribe(conn, &subscribe_params);
		return BT_GATT_ITER_STOP;
	}
	return BT_GATT_ITER_CONTINUE;
}

/* ===== MTU 协商回调（0x84 帧 242 字节，需要大 MTU） ===== */
static void mtu_cb(struct bt_conn *conn, uint8_t err,
		   struct bt_gatt_exchange_params *params)
{
	if (err) {
		printk("MTU exchange FAILED (%d)\n", err);
	} else {
		printk("MTU exchange OK, ATT MTU = %u\n", bt_gatt_get_mtu(conn));
	}
	/* MTU 协商完成后才开始 discover */
	discover_params.func = discover_func;
	discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	discover_params.start_handle = 1;
	discover_params.end_handle = 0xFFFF;
	discover_params.uuid = &uuid_write.uuid;
	bt_gatt_discover(conn, &discover_params);
}

static struct bt_gatt_exchange_params exchange_params = {
	.func = mtu_cb,
};

/* ===== 连接/断开 ===== */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connect FAILED (%d)\n", err);
		return;
	}
	eink_conn = conn;
	printk("Connected! Exchanging MTU...\n");
	/* 先协商大 MTU（0x84 帧 242 字节必须） */
	bt_gatt_exchange_mtu(conn, &exchange_params);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %d)\n", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ===== 扫描回调 ===== */
static void device_found(const bt_addr_le_t *addr, int8_t rssi,
			 uint8_t adv_type, struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	static const uint8_t zero[6] = {0};
	int err;

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Scan: %s (rssi=%d)\n", addr_str, rssi);

	if (memcmp(addr->a.val, zero, 6) == 0)
		return;
	if (memcmp(addr->a.val, eink_addr.a.val, 6) != 0)
		return;

	printk("Target found! Connecting...\n");
	bt_le_scan_stop();
	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				&conn_param, &eink_conn);
	if (err)
		printk("Create conn failed (%d)\n", err);
	else
		printk("Connect request sent, waiting...\n");
}

/* ===== 主函数 ===== */
int main(void)
{
	int err;

	printk("=== E-ink Central v3 (QuickLZ 压缩点阵) ===\n");
	/* 设备按 233B 步进读，0x83 包数 = ceil(len/233) */
	qlz_packets = (COMPRESSED_REV_LEN + 232) / 233;        /* type1 反转层 */
	qlz_packets2 = (COMPRESSED_IMAGE_LEN + 232) / 233;     /* type2 正层 */
	printk("Compressed image: type1(rev) %u B/%u pkts, type2(img) %u B/%u pkts (233B-aligned)\n",
	       (unsigned)qlz_len, (unsigned)qlz_packets,
	       (unsigned)qlz_len2, (unsigned)qlz_packets2);

	err = bt_addr_le_from_str(EINK_MAC_STR, "public", &eink_addr);
	if (err) {
		printk("MAC parse failed (%d)\n", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("bt_enable failed (%d)\n", err);
		return 0;
	}

	struct bt_le_scan_param scan_param = {
		.type       = BT_LE_SCAN_TYPE_ACTIVE,
		.options    = BT_LE_SCAN_OPT_NONE,
		.interval   = BT_GAP_SCAN_FAST_INTERVAL,
		.window     = BT_GAP_SCAN_FAST_WINDOW,
	};

	printk("Scanning for target %s...\n", EINK_MAC_STR);
	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Scan start failed (%d)\n", err);
		return 0;
	}

	for (;;)
		k_sleep(K_FOREVER);
}

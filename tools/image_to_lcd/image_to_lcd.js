/*
 * image_to_lcd.js —— 图片转 LCD 点阵工具（Node 版）v2
 *
 * v2 升级（2026-08-13，针对 128x296 三色墨水屏固件实测结论）：
 *   - 内置转置：内容按横屏 296x128 处理，输出竖屏 128x296 点阵（屏物理方向）
 *   - 默认极性对齐固件：黑像素 = 0、其他 = 1（type1 黑层数据，可直接压缩发送）
 *     —— 注意：固件是"0=有墨"，与常见"黑=1"相反！
 *   - --mode=red 预留红层：红像素 = 0、其他 = 1（type2 红层数据）
 *
 * 依赖：jimp（已装在本目录 node_modules）
 *
 * 用法：
 *   node image_to_lcd.js <图片路径> [选项]
 *
 * 选项：
 *   --lcd-w=128 --lcd-h=296   屏分辨率（输出点阵，默认 128x296）
 *   --content-w=296 --content-h=128   内容横屏尺寸（图片先缩到它，再转置，默认 296x128）
 *   --threshold=60            黑阈值：RGB 三通道都小于它才判黑（默认 60）
 *   --red-threshold=120       红阈值：R>它 且 G/B<它 判红（仅 red 模式，默认 120）
 *   --polarity=0              有墨像素的 bit 值（默认 0 = 固件极性；1 = 传统"黑=1"）
 *   --mode=black|red          取哪层：black=黑层（默认），red=红层（预留）
 *   --no-transpose            不转置（图片直接按屏分辨率铺，用于已是竖屏布局的图）
 *   --out=image_data.h        输出文件名（默认 image_data.h）
 *   --preview[=名字.png]      生成点阵还原图（按屏方向 128x296，验证取模对不对）
 *
 * 输出（image_data.h）：
 *   1) image_data[]           完整点阵（128x296 = 4736 字节，黑=0 白=1）
 *   2) 控制台打印 0x83 建议参数
 *   （切包由 main.c 的 build_pkt_table 按 QuickLZ 块边界处理，本工具不再切包）
 */

const fs = require('fs');
const path = require('path');
const { Jimp } = require('jimp');

/* ========== 命令行参数解析 ========== */
const args = process.argv.slice(2);
function getOpt(name, def) {
	const hit = args.find(a => a.startsWith('--' + name + '='));
	return hit ? hit.split('=')[1] : def;
}
const hasOpt = name => args.some(a => a === '--' + name || a.startsWith('--' + name + '='));

const inputPath = args.find(a => !a.startsWith('--'));
if (!inputPath) {
	console.error('用法: node image_to_lcd.js <图片路径> [--mode=black|red] [--lcd-w=128] [--lcd-h=296] [--polarity=0] [--out=data.h] [--preview]');
	process.exit(1);
}

/* 屏方向（输出点阵） */
const LCD_W   = parseInt(getOpt('lcd-w', 128), 10);    // 128
const LCD_H   = parseInt(getOpt('lcd-h', 296), 10);    // 296
/* 内容方向（横屏，图片先缩到这里再转置） */
const CONTENT_W = parseInt(getOpt('content-w', 296), 10);
const CONTENT_H = parseInt(getOpt('content-h', 128), 10);
const THRESH   = parseInt(getOpt('threshold', 60), 10);
const RED_THRESH = parseInt(getOpt('red-threshold', 120), 10);
const POLARITY = parseInt(getOpt('polarity', 0), 10);   /* 有墨像素的 bit 值，默认 0 = 固件极性 */
const MODE     = getOpt('mode', 'black');               /* black | red */
const TRANSPOSE = !hasOpt('no-transpose');
const OUT_FILE = getOpt('out', 'image_data.h');
const PREVIEW = hasOpt('preview');
const PREVIEW_FILE = getOpt('preview', 'preview.png');

const bytesPerRow = Math.ceil(LCD_W / 8);      /* 128 -> 16 */
const totalBytes  = bytesPerRow * LCD_H;       /* 16*296 = 4736 */

/* ========== 主流程 ========== */
(async () => {
	/* ① 读图 */
	const image = await Jimp.read(inputPath);
	console.log(`原始图片: ${image.width} x ${image.height}`);

	/* ② resize 到内容横屏尺寸（296x128，图片保持横向内容） */
	let img = image;
	if (image.width !== CONTENT_W || image.height !== CONTENT_H) {
		img = image.resize({ w: CONTENT_W, h: CONTENT_H });
		console.log(`内容已缩放到 ${CONTENT_W} x ${CONTENT_H}（横屏）`);
	}

	/* ③ 逐像素判定 → 写点阵（转置 + 极性） */
	const lcdData = Buffer.alloc(totalBytes);

	for (let y = 0; y < CONTENT_H; y++) {
		for (let x = 0; x < CONTENT_W; x++) {
			const c = img.getPixelColor(x, y);
			const r = (c >> 24) & 0xFF;
			const g = (c >> 16) & 0xFF;
			const b = (c >> 8) & 0xFF;

			let isInk = false;
			if (MODE === 'red') {
				/* 红层：R 高且 G/B 低判红 */
				isInk = (r > RED_THRESH && g < RED_THRESH && b < RED_THRESH);
			} else {
				/* 黑层：RGB 三通道都低判黑 */
				isInk = (r < THRESH && g < THRESH && b < THRESH);
			}
			const bitValue = isInk ? POLARITY : (1 - POLARITY);

			/* 写入点阵：
			 * 转置：竖屏点阵第 x 行（内容横屏的 x）第 y 列（内容横屏的 y）
			 *   byteIndex = x * bytesPerRow + (y >> 3)
			 * 不转置：第 y 行第 x 列
			 *   byteIndex = y * bytesPerRow + (x >> 3)
			 */
			const byteIndex = TRANSPOSE
				? (x * bytesPerRow + (y >> 3))
				: (y * bytesPerRow + (x >> 3));
			const bitPos = TRANSPOSE ? (7 - (y & 7)) : (7 - (x & 7));
			if (bitValue) {
				lcdData[byteIndex] |= (1 << bitPos);
			}
		}
	}

	/* ④ 输出 C 数组 */
	const sb = [];
	sb.push('/* 由 image_to_lcd.js v2 自动生成 */');
	sb.push(`/* 屏 ${LCD_W}x${LCD_H}  内容 ${CONTENT_W}x${CONTENT_H}  转置=${TRANSPOSE ? '是' : '否'}  模式=${MODE}  极性(有墨)=${POLARITY} */`);
	sb.push(`/* 每行 ${bytesPerRow} 字节，共 ${LCD_H} 行，总 ${totalBytes} 字节 */`);
	sb.push('');
	sb.push(`static const uint8_t image_data[${totalBytes}] = {`);
	for (let i = 0; i < totalBytes; i += 12) {
		const chunk = lcdData.slice(i, Math.min(i + 12, totalBytes));
		sb.push('\t' + [...chunk].map(b => '0x' + b.toString(16).toUpperCase().padStart(2, '0')).join(', ') + ',');
	}
	sb.push('};');
	sb.push('');

	/* 0x83 参数提示（作为注释） */
	sb.push('/* ===== 0x83 回复建议参数（配套本数据） ===== */');
	sb.push(`/* 文件大小 = ${totalBytes} (0x${totalBytes.toString(16).toUpperCase().padStart(4, '0')}) */`);
	sb.push('');

	fs.writeFileSync(path.join(process.cwd(), OUT_FILE), sb.join('\n'));
	console.log(`\n✅ 已生成 ${OUT_FILE}`);
	console.log(`   字节数: ${totalBytes}（${LCD_W} x ${LCD_H}，${bytesPerRow} 字节/行 x ${LCD_H} 行）`);
	console.log(`   模式: ${MODE}层  极性: 有墨像素=${POLARITY}`);
	console.log(`\n0x83 建议参数:`);
	console.log(`   文件大小: ${totalBytes}  (0x${totalBytes.toString(16).toUpperCase().padStart(4, '0')})`);
	console.log(`\n下一步: 用 qlz_tool_v2.exe 压缩生成 compressed_image.h，再编译烧录`);

	/* ⑤ 可选：点阵还原成 PNG（128x296 竖屏，验证取模对不对） */
	if (PREVIEW) {
		const preview = new Jimp({ width: LCD_W, height: LCD_H, color: 0xFFFFFFFF }); // 白底
		for (let py = 0; py < LCD_H; py++) {
			for (let px = 0; px < LCD_W; px++) {
				const byteIndex = py * bytesPerRow + (px >> 3);
				const bitPos = 7 - (px & 7);
				if ((lcdData[byteIndex] & (1 << bitPos)) === (POLARITY << bitPos)) {
					preview.setPixelColor(0x000000FF, px, py); // 有墨像素画黑（预览把墨显示为黑）
				}
			}
		}
		await preview.write(path.join(process.cwd(), PREVIEW_FILE));
		console.log(`\n✅ 点阵还原图已生成: ${PREVIEW_FILE}（${LCD_W}x${LCD_H} 竖屏，黑=有墨像素）`);
	}
})().catch(err => {
	console.error('出错了:', err.message);
	process.exit(1);
});

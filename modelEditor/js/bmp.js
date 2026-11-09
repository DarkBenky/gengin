// 24bpp BMP write/read matching util/saveImage.c: bottom-up rows, BGR, rows padded to 4 bytes.

"use strict";

const Bmp = {
	fileHeaderSize: 14,
	infoHeaderSize: 40,
	bytesPerPixel: 3,
};

function bmpRowStride(width) {
	return (width * Bmp.bytesPerPixel + 3) & ~3;
}

function writeBmp24(width, height, rgbTopDown) {
	const stride = bmpRowStride(width);
	const pixelBytes = stride * height;
	const total = Bmp.fileHeaderSize + Bmp.infoHeaderSize + pixelBytes;
	const out = new Uint8Array(total);
	const view = new DataView(out.buffer);

	out[0] = 0x42; // 'B'
	out[1] = 0x4D; // 'M'
	view.setUint32(2, total, true);
	view.setUint32(10, Bmp.fileHeaderSize + Bmp.infoHeaderSize, true);

	view.setUint32(14, Bmp.infoHeaderSize, true);
	view.setInt32(18, width, true);
	view.setInt32(22, height, true);
	view.setUint16(26, 1, true);
	view.setUint16(28, 24, true);
	view.setUint32(30, 0, true);
	view.setUint32(34, pixelBytes, true);
	view.setUint32(38, 0x0B13, true);
	view.setUint32(42, 0x0B13, true);

	const dataStart = Bmp.fileHeaderSize + Bmp.infoHeaderSize;
	for (let y = 0; y < height; y++) {
		const srcRow = (height - 1 - y) * width * 3;
		let dst = dataStart + y * stride;
		for (let x = 0; x < width; x++, dst += 3) {
			const src = srcRow + x * 3;
			out[dst] = rgbTopDown[src + 2];
			out[dst + 1] = rgbTopDown[src + 1];
			out[dst + 2] = rgbTopDown[src];
		}
	}
	return new Blob([out], { type: "image/bmp" });
}

// readBmp24 returns top-down RGB so it maps 1:1 onto the atlas texel order.
function readBmp24(arrayBuffer, fileName) {
	if (arrayBuffer.byteLength < Bmp.fileHeaderSize + Bmp.infoHeaderSize) {
		throw FormatError("not a BMP: only " + arrayBuffer.byteLength + " bytes");
	}
	const view = new DataView(arrayBuffer);
	if (view.getUint8(0) !== 0x42 || view.getUint8(1) !== 0x4D) {
		throw FormatError("not a BMP: missing BM signature");
	}
	const infoSize = view.getUint32(14, true);
	if (infoSize !== Bmp.infoHeaderSize) {
		throw FormatError("unsupported BMP header: expected a 40 byte BITMAPINFOHEADER, found " + infoSize);
	}
	const compression = view.getUint32(30, true);
	if (compression !== 0) throw FormatError("unsupported BMP: compression is not BI_RGB");
	const bitsPerPixel = view.getUint16(28, true);
	if (bitsPerPixel !== 24) throw FormatError("unsupported BMP: " + bitsPerPixel + " bits per pixel, expected 24");

	let width = view.getInt32(18, true);
	let height = view.getInt32(22, true);
	if (width <= 0) throw FormatError("corrupt BMP: width is " + width);
	let bottomUp = true;
	if (height < 0) { height = -height; bottomUp = false; }
	if (height === 0) throw FormatError("corrupt BMP: height is 0");

	const stride = bmpRowStride(width);
	const dataStart = view.getUint32(10, true);
	if (dataStart + stride * height > arrayBuffer.byteLength) {
		throw FormatError("truncated BMP: pixel data needs " + formatBytes(dataStart + stride * height) +
			" bytes but the file is " + formatBytes(arrayBuffer.byteLength));
	}

	const rgb = new Uint8Array(width * height * 3);
	const bytes = new Uint8Array(arrayBuffer);
	for (let row = 0; row < height; row++) {
		const srcRow = bottomUp ? height - 1 - row : row;
		let src = dataStart + srcRow * stride;
		let dst = row * width * 3;
		for (let x = 0; x < width; x++, src += 3, dst += 3) {
			rgb[dst] = bytes[src + 2];
			rgb[dst + 1] = bytes[src + 1];
			rgb[dst + 2] = bytes[src];
		}
	}
	return { width: width, height: height, rgb: rgb, fileName: fileName };
}

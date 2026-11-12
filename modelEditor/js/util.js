// Shared helpers: formatting, status line, chunked work so big files do not freeze the page.

"use strict";

const FormatError = function (message) {
	const err = new Error(message);
	err.name = "FormatError";
	return err;
};

function clamp(value, min, max) {
	if (value < min) return min;
	if (value > max) return max;
	return value;
}

function formatBytes(count) {
	if (count >= 1024 * 1024 * 1024) return (count / (1024 * 1024 * 1024)).toFixed(2) + " GiB";
	if (count >= 1024 * 1024) return (count / (1024 * 1024)).toFixed(1) + " MiB";
	if (count >= 1024) return (count / 1024).toFixed(1) + " KiB";
	return count + " B";
}

function formatCount(count) {
	return count.toLocaleString("en-US");
}

function baseNameOf(fileName) {
	const slash = Math.max(fileName.lastIndexOf("/"), fileName.lastIndexOf("\\"));
	const base = slash >= 0 ? fileName.slice(slash + 1) : fileName;
	const dot = base.lastIndexOf(".");
	return dot > 0 ? base.slice(0, dot) : base;
}

// workRange(start, end) is called with half-open index ranges; yielding between chunks keeps
// the status bar alive and lets the browser breathe on 300k triangle files.
function runChunked(total, chunkSize, workRange, onProgress) {
	return new Promise(function (resolve, reject) {
		let done = 0;
		function step() {
			try {
				const end = Math.min(done + chunkSize, total);
				if (done < end) workRange(done, end);
				done = end;
				if (onProgress) onProgress(done, total);
				if (done >= total) resolve();
				else setTimeout(step, 0);
			} catch (err) {
				reject(err);
			}
		}
		step();
	});
}

function readFileAsText(file) {
	return new Promise(function (resolve, reject) {
		const reader = new FileReader();
		reader.onload = function () { resolve(reader.result); };
		reader.onerror = function () { reject(new Error("Could not read " + file.name)); };
		reader.readAsText(file);
	});
}

function readFileAsArrayBuffer(file) {
	return new Promise(function (resolve, reject) {
		const reader = new FileReader();
		reader.onload = function () { resolve(reader.result); };
		reader.onerror = function () { reject(new Error("Could not read " + file.name)); };
		reader.readAsArrayBuffer(file);
	});
}

// Files dropped from a folder keep their relative path, which is how texture names from an MTL
// are matched without a directory listing.
function relativePathOf(file) {
	return file.webkitRelativePath || file.name;
}

function downloadBlob(blob, fileName) {
	const url = URL.createObjectURL(blob);
	const anchor = document.createElement("a");
	anchor.href = url;
	anchor.download = fileName;
	document.body.appendChild(anchor);
	anchor.click();
	document.body.removeChild(anchor);
	setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
}

// Saving never opens a picker: a linked folder is written directly, otherwise the blob goes
// out as a download. Exports can reach hundreds of MiB, so both paths stream the Blob as is.
async function saveBlob(blob, suggestedName, directoryHandle) {
	if (directoryHandle) {
		try {
			const fileHandle = await directoryHandle.getFileHandle(suggestedName, { create: true });
			const writable = await fileHandle.createWritable();
			await writable.write(blob);
			await writable.close();
			return suggestedName;
		} catch (err) {
			// a revoked or read only folder falls back to a download
		}
	}
	downloadBlob(blob, suggestedName);
	return suggestedName;
}

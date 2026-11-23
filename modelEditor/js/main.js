// Entry point: fill the atlas size choices and start the app.

"use strict";

function populateAtlasChoices(select) {
	for (let i = 0; i < Ui.atlasChoices.length; i++) {
		const option = document.createElement("option");
		option.value = String(Ui.atlasChoices[i]);
		option.textContent = Ui.atlasChoices[i] + " atlas";
		if (Ui.atlasChoices[i] === ObjImport.defaultTextureSize) option.selected = true;
		select.appendChild(option);
	}
}

function main() {
	populateAtlasChoices(document.getElementById("atlasSize"));
	const app = new App();
	try {
		app.init();
	} catch (err) {
		setStatus("editor failed to start: " + err.message, "error");
	}
	window.modelEditorApp = app;
}

main();

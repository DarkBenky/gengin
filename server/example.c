// Example: how to use the server
// This is not meant to compile — it shows the intended usage pattern.
//
// Design:
//   - Map/terrain is static: each client loads it locally, it is never synced.
//   - Only moving per-player objects (ships, projectiles, etc.) are synced.

#include "server.h"

// --- SERVER SIDE ---
//
// Start the server. Map objects are loaded locally by each client.
// ServerStart blocks and accepts connections. Per-player objects are
// created by clients via POST and expire automatically after 10 s of no updates.

void serverMain() {
	Server server;
	ServerInit(&server, 8080);
	ServerStart(&server);
	ServerStop(&server);
}

// --- CLIENT SIDE ---
//
// Load the map locally — no network needed for it.
// Only send/receive the objects that move (players, projectiles, etc.).
//
// Request  -> sent by client  (type + object data)
// Response -> received from server (object states, interpolated)
//
// POST id=0           -> create new object, server returns assigned id
// POST id=myId        -> update existing object position/rotation
// POST id=myId + ""   -> delete the object (empty filename = remove)
// GET                 -> fetch all per-player objects currently on the server

void clientMain() {
	loadMapLocally("assets/models/map.obj"); // each client does this themselves

	int sock = connectToServer("127.0.0.1", 8080);

	// --- Spawn this player's object ---
	// id=0 means "create new", server assigns and returns the id
	Request spawn = {
		.type = POST,
		.id = 0,
		.timeStemp = (uint32)time(NULL),
		.position = {10, 0, 5},
		.rotation = {0, 0, 0},
		.scale = {1, 1, 1},
	};
	strncpy(spawn.filename, "assets/models/r27.obj", sizeof(spawn.filename) - 1);
	send(sock, &spawn, sizeof(Request), 0);

	uint32 myPlayerId;
	recv(sock, &myPlayerId, sizeof(uint32), 0);

	// --- Each frame: send updated position ---
	Request update = {
		.type = POST,
		.id = myPlayerId,
		.timeStemp = (uint32)time(NULL),
		.position = {10.5f, 0, 5},
		.rotation = {0, 0.1f, 0},
		.scale = {1, 1, 1},
	};
	strncpy(update.filename, "assets/models/r27.obj", sizeof(update.filename) - 1);
	send(sock, &update, sizeof(Request), 0);

	uint32 unused;
	recv(sock, &unused, sizeof(uint32), 0);

	// --- Each frame: GET all other players' positions ---
	Request get = {.type = GET, .timeStemp = (uint32)time(NULL)};
	send(sock, &get, sizeof(Request), 0);

	uint32 count;
	recv(sock, &count, sizeof(uint32), 0);

	Response objects[count];
	recv(sock, objects, count * sizeof(Response), 0);

	for (uint32 i = 0; i < count; i++) {
		// position/rotation are already interpolated by the server
		render(objects[i].filename, objects[i].position, objects[i].rotation, objects[i].scale);
	}

	// --- On disconnect: delete this player's object ---
	Request del = {
		.type = POST,
		.id = myPlayerId,
		.timeStemp = (uint32)time(NULL),
		.filename = "", // empty = delete
	};
	send(sock, &del, sizeof(Request), 0);
	recv(sock, &unused, sizeof(uint32), 0);

	close(sock);
}

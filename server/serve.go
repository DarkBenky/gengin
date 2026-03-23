package main

import (
	"log/slog"
	"sync"

	"github.com/labstack/echo/v5"
	"github.com/labstack/echo/v5/middleware"
)

var serverState ServerState

type float3 struct {
	X float64 `json:"x"`
	Y float64 `json:"y"`
	Z float64 `json:"z"`
}

type Object struct {
	Id        uint32   `json:"id"`
	TimeStamp uint32   `json:"time_stamp"`
	FileName  [32]byte `json:"file_name"`
	Position  float3   `json:"position"`
	Rotation  float3   `json:"rotation"`
	Scale     float3   `json:"scale"`
}

type ServerState struct {
	mu      sync.RWMutex
	Objects map[uint32]Object
}

func cleanOldObjects(state *ServerState, keepTime uint32) {
	state.mu.Lock()
	defer state.mu.Unlock()

	for id, obj := range state.Objects {
		if obj.TimeStamp < keepTime {
			delete(state.Objects, id)
		}
	}
}

func addObject(state *ServerState, obj Object) {
	state.mu.Lock()
	defer state.mu.Unlock()

	if state.Objects == nil {
		state.Objects = make(map[uint32]Object)
	}
	if existing, exists := state.Objects[obj.Id]; exists {
		// check if new timestamp is greater than existing timestamp
		if obj.TimeStamp > existing.TimeStamp {
			state.Objects[obj.Id] = obj
		}
	} else {
		state.Objects[obj.Id] = obj
	}
}

func PostAddObject(c *echo.Context) error {
	var obj Object
	if err := c.Bind(&obj); err != nil {
		slog.Error("failed to bind request body to Object struct", "error", err)
		return c.JSON(400, map[string]string{"error": "invalid request body"})
	}

	addObject(&serverState, obj)
	return c.JSON(200, map[string]string{"status": "success"})
}

func GetObjects(c *echo.Context) error {
	serverState.mu.RLock()
	objects := make([]Object, 0, len(serverState.Objects))
	for _, obj := range serverState.Objects {
		objects = append(objects, obj)
	}
	serverState.mu.RUnlock()

	return c.JSON(200, objects)
}

func main() {
	// Echo instance
	e := echo.New()

	// Middleware
	e.Use(middleware.RequestLogger()) // use the RequestLogger middleware with slog logger
	e.Use(middleware.Recover())       // recover panics as errors for proper error handling

	// Routes
	e.POST("/addObject", PostAddObject)
	e.GET("/getObjects", GetObjects)

	// Start server
	if err := e.Start(":8080"); err != nil {
		slog.Error("failed to start server", "error", err)
	}
}

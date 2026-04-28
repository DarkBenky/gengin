## TODO

- [ ] Plane editor
    - [ ] export
    - [ ] import to c
    - [ ] implement c wing simulation
- [X] implement the simplified simulation in 2d then move to c
    - [X] move python code to c
    - [ ] train model
    - [ ] integrate the model
- [ ] render directly wia open gl not c => open gl => minifb

- [X] sync all object
    - [X] why movement si so jerky (jumping around)
- [X] Server Synchronization
    - [X] crete simple project that will test diffrent methods
        - [X] TCP server
        - [X] we can simplify it we crete n planes on each client for while there are not used they are invisible and when user connect one of the planes will be given to user synchronization will work like this
        ```
        on innit => get free plane
        on update => send users plane state and receive new state we can add velocity to each plane so the state will be interpolated between updates
        on close => set planes as free and invisible
    - [ ] integrate it to main.c

- [ ] textures as post proces step on gpu we need to add uv mapping we shoulde use per triangle texture (normal, albedo ...)
    - [ ] input
        - [ ] 2d screen buffers (G-buffer)
            - [ ] Albedo map (HDR above 1 = lit) <- rendered scene color
            - [ ] World space normal map
            - [X] UV map <- done calculated on cpu
            - [ ] Texture ID map
            - [ ] Depth map

        - [ ] light
            - [ ] sun direction
            - [ ] light intensity

        - [ ] texture atlas
            - [ ] Blend Factor
            - [ ] Albedo <- source texture (blend wit base color)
            - [ ] Normal map
            - [ ] Roughness
            - [ ] Metallic

- [ ] model editor / object editor / texture editor
- [X] 1. create generic server (async) client (async) and then use lib that is client and server side for model loading updating etc ...
    - [X] reqest designe
    ```c
    #typedef struct {
        uint32 Size
        int Id
        type Type // POST => no repliy, GET => repley
        uint8 data
    } Reqest;
    ```
- [X] God Rays
- [X] Emission
    - [X] crete emission map for each object
    ![img](./render.png)
- [X] Shadows same as reflection
- [X] Clouds
- [X] Bloom
    - [X] Too slow

- [X] Replace screen space refection by raytraced onece
    - [X] use lower resolution and blur row apply it to frame buffer
        - [X] apply direct reflection [red][green][blue][roughness]
            - [X] we blur based on 4th channel

- [ ] GPU rendering (keep it simple — port current CPU pipeline (**later**)
    - [X] clouds
    - [X] god rays

- [ ] Plane controls
  - [ ] Use something like this but we will simplified it
    - [ ] Example : ![c_heder](examplePlaneStruct.h)
    - [ ] Flight model should be physics-based only — derived values like turn rate should not be hardcoded constants
    - [ ] Control by providing a target nose vector (like War Thunder)
      - [ ] Add damping to controls to avoid oscillations

- [ ] Radar / heat seeker simulation for missiles
  - [ ] Simulate radar scanning by sampling object ID buffer over a small cone area and computing RCS on hits
    - [ ] Non-Doppler radar: average terrain clutter hits with target hits to simulate ground return noise
    - [ ] Doppler radar: filter out stationary objects (terrain), track only moving targets (missiles, planes); requires relative velocity per object

- [ ] Missile guidance and control

- [X] Server integration for multiplayer

- [X] Add screen space reflection

- [x] Test if using multiple rows per ray trace task improves performance (e.g. 8 rows per task)
  - Tested: it is better to use one task per row when there is a lot of work (**more work == fewer rows per task**, **less work == more rows per task**)
    - ![results](results.md)

## Current Render

![img](./img.png)

- [ ] Airofoil and flaps simulation 
    - ![example_c_implementation](planeSurfacesExample.c)

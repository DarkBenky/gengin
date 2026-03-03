## TODO

- [ ] Plane controls
- [ ] Radar / heat seeker simulation for missiles
    - [ ] Simulate radar scanning by sampling object ID buffer over a small cone area and computing RCS on hits
        - [ ] Non-Doppler radar: average terrain clutter hits with target hits to simulate ground return noise
        - [ ] Doppler radar: filter out stationary objects (terrain), track only moving targets (missiles, planes); requires relative velocity per object
- [ ] Missile guidance and control
- [ ] Server integration for multiplayer
- [ ] GPU rendering (keep it simple — port current CPU pipeline)
- [ ] Add Screen space reflection
- [ ] Test if using multiple row in ray trace is not fast for example 8 row one task

- Current Render
    - ![img](./img.png)

import pygame
import math

class Float2:
    def __init__(self, x, y):
        self.x = x
        self.y = y

def _atmosphere(altitude):
    """ISA atmosphere. Returns (air_density kg/m^3, speed_of_sound m/s)."""
    altitude = max(0.0, min(altitude, 20000.0))
    if altitude <= 11000.0:
        T = 288.15 - 0.0065 * altitude
        p = 101325.0 * (T / 288.15) ** 5.2561
    else:
        T = 216.65
        p = 22632.1 * math.exp(-0.0001577 * (altitude - 11000.0))
    rho = p / (287.05 * T)
    a = math.sqrt(1.4 * 287.05 * T)
    return rho, a

class Wing:
    def __init__(self, position, size, angle,
                 liftCoefficient=1.0, dragCoefficient=0.1,
                 surfaceArea=1.0, aspectRatio=6.0,
                 efficiency=0.9, stallAngle=15.0,
                 camber=0.04, altitude=0.0):
        self.position = position
        self.size = size
        self.angle = angle
        self.liftCoefficient = liftCoefficient
        self.dragCoefficient = dragCoefficient
        self.surfaceArea = surfaceArea
        self.aspectRatio = aspectRatio
        self.efficiency = efficiency
        self.stallAngle = stallAngle
        self.camber = camber
        self.altitude = altitude

    def draw(self, screen):
        end_x = self.position[0] + self.size * math.cos(math.radians(self.angle))
        end_y = self.position[1] + self.size * math.sin(math.radians(self.angle))
        pygame.draw.line(screen, (255, 255, 255), self.position, (end_x, end_y), 2)

    def _lift(self, airspeed):
        aoa = self.angle
        airDensity, speedOfSound = _atmosphere(self.altitude)
        mach = airspeed / speedOfSound
        dynamicPressure = 0.5 * airDensity * airspeed**2

        stallFactor = 0.3 if abs(aoa) > self.stallAngle else 1.0

        baseCL = (self.liftCoefficient * math.sin(math.radians(aoa)) + 2 * math.pi * self.camber) * stallFactor

        # Prandtl-Glauert compressibility correction (subsonic). Capped at M=0.85 where it breaks down.
        if mach < 0.85:
            beta = math.sqrt(max(1.0 - mach**2, 1e-4))
            compressibility = 1.0 / beta
        else:
            compressibility = 1.0 / math.sqrt(1.0 - 0.85**2)

        effectiveLiftCoeff = baseCL * compressibility

        parasiticCD = self.dragCoefficient * math.cos(math.radians(aoa))

        # Wave drag: rises steeply through transonic region, then follows linearised supersonic theory
        if mach < 0.8:
            waveCd = 0.0
        elif mach < 1.2:
            waveCd = 0.5 * ((mach - 0.8) / 0.4) ** 3
        else:
            waveCd = 0.5 / math.sqrt(max(mach**2 - 1.0, 0.01))

        parasiticDrag = dynamicPressure * self.surfaceArea * (parasiticCD + waveCd)

        liftMagnitude = dynamicPressure * self.surfaceArea * effectiveLiftCoeff
        inducedDrag = liftMagnitude**2 / (dynamicPressure * math.pi * self.aspectRatio * self.efficiency * self.surfaceArea + 1e-6)
        dragMagnitude = parasiticDrag + inducedDrag

        liftVector = (
            liftMagnitude * math.cos(math.radians(aoa - 90)),
            liftMagnitude * math.sin(math.radians(aoa - 90))
        )
        dragVector = (
            dragMagnitude * math.cos(math.radians(aoa)),
            dragMagnitude * math.sin(math.radians(aoa))
        )

        # Center of pressure (~25% chord, approximated as midpoint)
        midX = self.position[0] + self.size * math.cos(math.radians(aoa)) / 2
        midY = self.position[1] + self.size * math.sin(math.radians(aoa)) / 2
        liftCenter = (midX, midY)
        dragCenter = (midX, midY)

        return liftVector, liftMagnitude, liftCenter, dragVector, dragMagnitude, dragCenter, mach, airDensity, speedOfSound

class Arrow:
    def __init__(self, position, size, angle, color=(255, 0, 0)):
        self.position = position
        self.size = size
        self.angle = angle
        self.color = color

    def draw(self, screen):
        end_x = self.position[0] + self.size * math.cos(math.radians(self.angle))
        end_y = self.position[1] + self.size * math.sin(math.radians(self.angle))

        pygame.draw.line(screen, self.color, self.position, (end_x, end_y), 2)
        pygame.draw.polygon(screen, self.color, [(end_x, end_y),
                                                    (end_x - 5 * math.cos(math.radians(self.angle - 150)), end_y - 5 * math.sin(math.radians(self.angle - 150))),
                                                    (end_x - 5 * math.cos(math.radians(self.angle + 150)), end_y - 5 * math.sin(math.radians(self.angle + 150)))])

PROFILES = [
    {"name": "NACA 2412",        "liftCoefficient": 1.2,  "dragCoefficient": 0.02,  "surfaceArea": 1.0, "aspectRatio": 7.0,  "efficiency": 0.90, "stallAngle": 16.0, "camber": 0.02},
    {"name": "Flat plate",        "liftCoefficient": 0.8,  "dragCoefficient": 0.10,  "surfaceArea": 1.0, "aspectRatio": 5.0,  "efficiency": 0.75, "stallAngle": 10.0, "camber": 0.00},
    {"name": "Thick symmetric",   "liftCoefficient": 1.0,  "dragCoefficient": 0.04,  "surfaceArea": 1.0, "aspectRatio": 6.0,  "efficiency": 0.85, "stallAngle": 14.0, "camber": 0.00},
    {"name": "High-lift",         "liftCoefficient": 1.8,  "dragCoefficient": 0.05,  "surfaceArea": 1.0, "aspectRatio": 9.0,  "efficiency": 0.92, "stallAngle": 18.0, "camber": 0.06},
    {"name": "Delta wing",        "liftCoefficient": 0.6,  "dragCoefficient": 0.15,  "surfaceArea": 1.0, "aspectRatio": 2.0,  "efficiency": 0.60, "stallAngle": 30.0, "camber": 0.00},
    {"name": "Cambered wing",     "liftCoefficient": 1.4,  "dragCoefficient": 0.03,  "surfaceArea": 1.0, "aspectRatio": 8.0,  "efficiency": 0.88, "stallAngle": 20.0, "camber": 0.04},
    {"name": "Supercritical",     "liftCoefficient": 1.0,  "dragCoefficient": 0.02,  "surfaceArea": 1.0, "aspectRatio": 7.0,  "efficiency": 0.95, "stallAngle": 12.0, "camber": 0.01},
    {"name": "Low aspect ratio",  "liftCoefficient": 0.9,  "dragCoefficient": 0.08,  "surfaceArea": 1.0, "aspectRatio": 3.0,  "efficiency": 0.70, "stallAngle": 25.0, "camber": 0.02},
    # missile / fighter surfaces
    {"name": "Missile fin",       "liftCoefficient": 0.5,  "dragCoefficient": 0.20,  "surfaceArea": 0.3, "aspectRatio": 1.0,  "efficiency": 0.55, "stallAngle": 45.0, "camber": 0.00},
    {"name": "Missile canard",    "liftCoefficient": 0.7,  "dragCoefficient": 0.18,  "surfaceArea": 0.4, "aspectRatio": 1.5,  "efficiency": 0.60, "stallAngle": 40.0, "camber": 0.00},
    {"name": "F-16 clipped delta","liftCoefficient": 1.1,  "dragCoefficient": 0.025, "surfaceArea": 1.0, "aspectRatio": 3.2,  "efficiency": 0.82, "stallAngle": 28.0, "camber": 0.01},
    {"name": "F-22 cranked arrow","liftCoefficient": 1.05, "dragCoefficient": 0.020, "surfaceArea": 1.0, "aspectRatio": 2.4,  "efficiency": 0.85, "stallAngle": 32.0, "camber": 0.00},
    {"name": "Su-27 ogival delta","liftCoefficient": 1.15, "dragCoefficient": 0.022, "surfaceArea": 1.0, "aspectRatio": 2.8,  "efficiency": 0.83, "stallAngle": 35.0, "camber": 0.01},
    {"name": "Strake",            "liftCoefficient": 0.4,  "dragCoefficient": 0.30,  "surfaceArea": 0.5, "aspectRatio": 0.8,  "efficiency": 0.50, "stallAngle": 50.0, "camber": 0.00},
]

def apply_profile(wing, profile):
    wing.liftCoefficient = profile["liftCoefficient"]
    wing.dragCoefficient = profile["dragCoefficient"]
    wing.surfaceArea     = profile["surfaceArea"]
    wing.aspectRatio     = profile["aspectRatio"]
    wing.efficiency      = profile["efficiency"]
    wing.stallAngle      = profile["stallAngle"]
    wing.camber          = profile["camber"]

def force_px(f):
    """Log-scale force to pixels so the arrows stay on-screen across the Mach range."""
    return math.copysign(math.log1p(abs(f)) * 25, f)

if __name__ == "__main__":
    pygame.init()
    screen = pygame.display.set_mode((800, 600))
    font = pygame.font.SysFont(None, 22)
    clock = pygame.time.Clock()

    profile_index = 0
    airspeed  = 50.0    # m/s
    altitude  = 0.0     # m
    wing = Wing((400, 300), 100, 180)
    apply_profile(wing, PROFILES[profile_index])
    liftArrow        = Arrow((400, 300), 100, -90, (0, 255, 0))
    dragArrow        = Arrow((400, 300), 100, 180, (255, 0, 0))
    combinedArrow    = Arrow((400, 300), 100, 0,   (255, 255, 0))
    windDirectionArrow = Arrow((650, 300), -100, 0, (0, 0, 255))

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_q:
                    profile_index = (profile_index + 1) % len(PROFILES)
                    apply_profile(wing, PROFILES[profile_index])
                elif event.key == pygame.K_UP:
                    altitude = min(altitude + 500.0, 20000.0)
                elif event.key == pygame.K_DOWN:
                    altitude = max(altitude - 500.0, 0.0)
                elif event.key == pygame.K_RIGHT:
                    airspeed = min(airspeed + 10.0, 700.0)
                elif event.key == pygame.K_LEFT:
                    airspeed = max(airspeed - 10.0, 10.0)

        keys = pygame.key.get_pressed()
        if keys[pygame.K_w]:
            wing.angle += 1
        if keys[pygame.K_s]:
            wing.angle -= 1

        wing.altitude = altitude

        screen.fill((0, 0, 0))
        wing.draw(screen)
        liftVector, liftMag, liftCenter, dragVector, dragMag, dragCenter, mach, rho, sos = wing._lift(airspeed)

        liftArrow.angle    = math.degrees(math.atan2(liftVector[1], liftVector[0]))
        liftArrow.size     = force_px(liftMag)
        liftArrow.position = liftCenter
        dragArrow.angle    = math.degrees(math.atan2(dragVector[1], dragVector[0]))
        dragArrow.size     = force_px(dragMag)
        dragArrow.position = dragCenter

        combinedVector    = (liftVector[0] + dragVector[0], liftVector[1] + dragVector[1])
        combinedMag       = math.hypot(combinedVector[0], combinedVector[1])
        combinedArrow.size     = force_px(combinedMag)
        combinedArrow.angle    = math.degrees(math.atan2(combinedVector[1], combinedVector[0]))
        combinedArrow.position = liftCenter

        liftArrow.draw(screen)
        dragArrow.draw(screen)
        combinedArrow.draw(screen)
        windDirectionArrow.draw(screen)
        wing.draw(screen)

        # HUD
        regime = "supersonic" if mach >= 1.2 else "transonic" if mach >= 0.8 else "subsonic"
        hud = [
            f"Profile (Q):   {PROFILES[profile_index]['name']}",
            f"Airspeed (</>) {airspeed:.0f} m/s  ({airspeed * 3.6:.0f} km/h)",
            f"Altitude (^/v) {altitude:.0f} m",
            f"Mach:          {mach:.3f}  [{regime}]",
            f"Air density:   {rho:.4f} kg/m^3",
            f"Speed of sound:{sos:.1f} m/s",
            f"Lift:          {liftMag:.1f} N",
            f"Drag:          {dragMag:.1f} N",
            f"L/D:           {liftMag / (dragMag + 1e-6):.2f}",
        ]
        for i, line in enumerate(hud):
            surf = font.render(line, True, (200, 200, 200))
            screen.blit(surf, (10, 10 + i * 24))

        pygame.display.flip()
        clock.tick(60)
        
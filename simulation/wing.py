import pygame
import math

class Float2:
    def __init__(self, x, y):
        self.x = x
        self.y = y

class Wing:
    def __init__(self, position, size, angle,
                 liftCoefficient=1.0, dragCoefficient=0.1,
                 surfaceArea=1.0, aspectRatio=6.0,
                 efficiency=0.9, stallAngle=15.0,
                 camber=0.04):
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

    def draw(self, screen):
        end_x = self.position[0] + self.size * math.cos(math.radians(self.angle))
        end_y = self.position[1] + self.size * math.sin(math.radians(self.angle))
        pygame.draw.line(screen, (255, 255, 255), self.position, (end_x, end_y), 2)

    def _lift(self, windSpeed):
        aoa = self.angle
        airDensity = 1.225
        dynamicPressure = 0.5 * airDensity * windSpeed**2

        # Stall check
        if abs(aoa) > self.stallAngle:
            stallFactor = 0.3
        else:
            stallFactor = 1.0

        # Camber adds baseline lift even at 0° AoA, and increases lift at small angles, but reduces it at high angles due to flow separation
        effectiveLiftCoeff = (self.liftCoefficient * math.sin(math.radians(aoa)) + 2 * math.pi * self.camber) * stallFactor

        # Parasitic (form) drag increases with AoA due to increased frontal area and flow separation, but is reduced at very high angles due to stall
        parasiticDrag = dynamicPressure * self.surfaceArea * self.dragCoefficient * math.cos(math.radians(aoa))

        # Lift magnitude 
        liftMagnitude = dynamicPressure * self.surfaceArea * effectiveLiftCoeff

        # Induced drag (from lift generation) increases with lift and decreases with aspect ratio and efficiency, but is reduced at high angles due to stall
        inducedDrag = liftMagnitude**2 / (dynamicPressure * math.pi * self.aspectRatio * self.efficiency * self.surfaceArea + 1e-6)

        dragMagnitude = parasiticDrag + inducedDrag

        # Vectors
        liftVector = (
            liftMagnitude * math.cos(math.radians(aoa - 90)),
            liftMagnitude * math.sin(math.radians(aoa - 90))
        )
        dragVector = (
            dragMagnitude * math.cos(math.radians(aoa)),
            dragMagnitude * math.sin(math.radians(aoa))
        )

        # Center of pressure (acts at ~25% chord, approximated as midpoint here)
        midX = self.position[0] + self.size * math.cos(math.radians(aoa)) / 2
        midY = self.position[1] + self.size * math.sin(math.radians(aoa)) / 2
        liftCenter = (midX, midY)
        dragCenter = (midX, midY)

        return liftVector, liftMagnitude, liftCenter, dragVector, dragMagnitude, dragCenter

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

if __name__ == "__main__":
    pygame.init()
    screen = pygame.display.set_mode((800, 600))
    font = pygame.font.SysFont(None, 24)
    clock = pygame.time.Clock()

    profile_index = 0
    wing = Wing((400, 300), 100, 180)
    apply_profile(wing, PROFILES[profile_index])
    liftArrow = Arrow((400, 300), 200, -90, (0, 255, 0))
    dragArrow = Arrow((400, 300), 200, 180, (255, 0, 0))
    combinedArrow = Arrow((400, 300), 200, 0, (255, 255, 0))
    windDirectionArrow = Arrow((400+250, 300), -100, 0, (0, 0, 255))

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_q:
                    profile_index = (profile_index + 1) % len(PROFILES)
                    apply_profile(wing, PROFILES[profile_index])

        keys = pygame.key.get_pressed()
        if keys[pygame.K_w]:
            wing.angle += 1
        if keys[pygame.K_s]:
            wing.angle -= 1


        screen.fill((0, 0, 0))
        wing.draw(screen)
        liftVector, liftMagnitude, liftCenter, dragVector, dragMagnitude, dragCenter = wing._lift(50)
        
        liftArrow.angle = math.degrees(math.atan2(liftVector[1], liftVector[0]))
        liftArrow.size = liftMagnitude
        liftArrow.position = liftCenter
        dragArrow.angle = math.degrees(math.atan2(dragVector[1], dragVector[0]))
        dragArrow.size = dragMagnitude
        dragArrow.position = dragCenter

        combinedVector = (liftVector[0] + dragVector[0], liftVector[1] + dragVector[1])
        combinedMagnitude = math.hypot(combinedVector[0], combinedVector[1])
        combinedAngle = math.degrees(math.atan2(combinedVector[1], combinedVector[0]))
        combinedArrow.size = combinedMagnitude
        combinedArrow.angle = combinedAngle 
        combinedArrow.position = liftCenter

        liftArrow.draw(screen)
        dragArrow.draw(screen)
        combinedArrow.draw(screen)
        windDirectionArrow.draw(screen)
        wing.draw(screen)

        label = font.render(f"Profile (Q): {PROFILES[profile_index]['name']}", True, (200, 200, 200))
        screen.blit(label, (10, 10))

        pygame.display.flip()
        clock.tick(60)
        
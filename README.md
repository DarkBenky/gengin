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
    - Example
    ```cl
    float3 sampleScreenSpaceReflectionFiltered(
    	__global const float *ScreenColors,
    	__global const float *ScreenDistances,
    	const float3 rayOrigin,
    	const float3 rayDirection,
    	const float3 camPos,
    	const float3 camDir,
    	const float fov,
    	const int screenWidth,
    	const int screenHeight,
    	const float maxDistance,
    	const int maxSteps,
    	const float stepSize) {
    	float3 fallbackColor = (float3)(0.0f, 0.0f, 0.0f);
    
    	float3 forward = normalize(camDir);
    	float3 camUp = (float3)(0.0f, 1.0f, 0.0f);
    	float3 right = normalize(cross(forward, camUp));
    	float3 up = cross(right, forward);
    
    	float3 currentPos = rayOrigin;
    	float distanceTraveled = 0.0f;
    
    	// FIX 1: Start with a small offset to avoid self-intersection
    	currentPos += rayDirection * stepSize * 0.5f;
    
    	for (int step = 0; step < maxSteps; step++) {
    		currentPos += rayDirection * stepSize;
    		distanceTraveled += stepSize;
    
    		if (distanceTraveled > maxDistance) {
    			break;
    		}
    
    		float3 relativePos = currentPos - camPos;
    		float depth = dot(relativePos, forward);
    
    		// FIX 2: Better depth bounds checking
    		if (depth <= 0.01f || depth > maxDistance) {
    			continue;
    		}
    
    		float fovScale = 1.0f / (depth * fov);
    		float screenRight = dot(relativePos, right) * fovScale;
    		float screenUpward = dot(relativePos, up) * fovScale;
    
    		float halfWidth = screenWidth * 0.5f;
    		float halfHeight = screenHeight * 0.5f;
    
    		float screenX = screenRight * halfWidth + halfWidth;
    		float screenY = -screenUpward * halfHeight + halfHeight;
    
    		// FIX 3: Add margin to screen bounds to avoid edge artifacts
    		if (screenX < 1.0f || screenX >= (screenWidth - 1.0f) ||
    			screenY < 1.0f || screenY >= (screenHeight - 1.0f)) {
    			continue;
    		}
    
    		// Get integer coordinates for depth test
    		int pixelX = (int)screenX;
    		int pixelY = (int)screenY;
    		int pixelIndex = pixelY * screenWidth + pixelX;
    
    		// FIX 4: Bounds check for pixelIndex
    		if (pixelIndex < 0 || pixelIndex >= screenWidth * screenHeight) {
    			continue;
    		}
    
    		float sceneDepth = ScreenDistances[pixelIndex];
    
    		// FIX 5: Better depth comparison with adaptive threshold
    		float depthThreshold = stepSize * 1.5f + depth * 0.001f; // Adaptive threshold
    		float depthDifference = depth - sceneDepth;
    
    		// FIX 6: Check if we've hit something and it's in front of our ray
    		if (sceneDepth > 0.01f && depthDifference > 0.0f && depthDifference < depthThreshold) {
    			// FIX 7: Improved bilinear filtering with bounds checking
    			float fx = screenX - pixelX;
    			float fy = screenY - pixelY;
    
    			// Sample 4 neighboring pixels with bounds checking
    			int x0 = clamp(pixelX, 0, screenWidth - 1);
    			int x1 = clamp(pixelX + 1, 0, screenWidth - 1);
    			int y0 = clamp(pixelY, 0, screenHeight - 1);
    			int y1 = clamp(pixelY + 1, 0, screenHeight - 1);
    
    			int idx00 = (y0 * screenWidth + x0) * 3;
    			int idx10 = (y0 * screenWidth + x1) * 3;
    			int idx01 = (y1 * screenWidth + x0) * 3;
    			int idx11 = (y1 * screenWidth + x1) * 3;
    
    			// FIX 8: Check all sample indices are valid
    			if (idx00 >= 0 && idx11 < screenWidth * screenHeight * 3) {
    				// Interpolate colors
    				float3 color00 = (float3)(ScreenColors[idx00], ScreenColors[idx00 + 1], ScreenColors[idx00 + 2]);
    				float3 color10 = (float3)(ScreenColors[idx10], ScreenColors[idx10 + 1], ScreenColors[idx10 + 2]);
    				float3 color01 = (float3)(ScreenColors[idx01], ScreenColors[idx01 + 1], ScreenColors[idx01 + 2]);
    				float3 color11 = (float3)(ScreenColors[idx11], ScreenColors[idx11 + 1], ScreenColors[idx11 + 2]);
    
    				float3 colorTop = mix(color00, color10, fx);
    				float3 colorBottom = mix(color01, color11, fx);
    				float3 finalColor = mix(colorTop, colorBottom, fy);
    
    				// FIX 9: Ensure we return a valid color (not black)
    				if (length(finalColor) > 0.01f) {
    					return finalColor;
    				}
    			}
    		}
    	}
    
    	return fallbackColor;
    }```
- [ ] Test if using multiple row in ray trace is not fast for example 8 row one task

- Current Render
    - ![img](./img.png)

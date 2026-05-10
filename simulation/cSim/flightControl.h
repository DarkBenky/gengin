// hand made algo to find good surface control to target point in 3d space
// using iterative approaches where we try to minimize the loss based on this
// 1. try to apply roll so the top of the plane will be facing the target
// 2. apply this algo to pitch
// pitch = 0.5  // neutral
// for try in range(tries):
//     saveState()
//     apply(pitch)
//     for step in range(lookahead):
//         simStep()
//     nextLoss = distanceToTarget
//     restoreState()          // re-evaluate from same point each try

//     currentLoss = distanceToTarget
//     if nextLoss < currentLoss:
//         pitch *= 1.125
//     else:
//         pitch /= 1.125
// 3. try to same algo for yaw






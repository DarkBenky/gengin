import csv
import sys
import math
from pathlib import Path

def analyze_csv(path):
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    
    if not rows:
        return None
    
    distances = [float(r['DistanceToTarget']) for r in rows]
    min_dist = min(distances)
    min_idx = distances.index(min_dist)
    final_dist = distances[-1]
    
    # Distance improvement per step (negative = closing, positive = diverging)
    dist_deltas = [distances[i+1] - distances[i] for i in range(len(distances)-1)]
    avg_closing = sum(d for d in dist_deltas if d < 0) / max(1, sum(1 for d in dist_deltas if d < 0))
    avg_diverging = sum(d for d in dist_deltas if d > 0) / max(1, sum(1 for d in dist_deltas if d > 0))
    
    # How many steps to first reach within 500m, 200m, 100m?
    first_500 = next((i for i, d in enumerate(distances) if d < 500), None)
    first_200 = next((i for i, d in enumerate(distances) if d < 200), None)
    first_100 = next((i for i, d in enumerate(distances) if d < 100), None)
    
    # Overshoot magnitude: how much does it diverge after min
    post_min_max = max(distances[min_idx:]) if min_idx < len(distances) else distances[-1]
    overshoot = post_min_max - min_dist
    
    # Speed at closest approach (approximate from position deltas)
    if min_idx < len(rows):
        px = float(rows[min_idx]['PlaneVelocityX'])
        py = float(rows[min_idx]['PlaneVelocityY'])
        pz = float(rows[min_idx]['PlaneVelocityZ'])
        speed_at_min = math.sqrt(px*px + py*py + pz*pz)
    else:
        speed_at_min = 0
    
    return {
        'path': path,
        'steps': len(rows),
        'min_dist': min_dist,
        'min_idx': min_idx,
        'final_dist': final_dist,
        'avg_closing': avg_closing,
        'avg_diverging': avg_diverging,
        'first_500': first_500,
        'first_200': first_200,
        'first_100': first_100,
        'overshoot': overshoot,
        'speed_at_min': speed_at_min,
        'distances': distances,
    }


def main():
    csv_dir = Path('simulation/cSim')
    csv_files = sorted(csv_dir.glob('flightControlLogs*.csv'))
    
    if not csv_files:
        print("No CSV files found in simulation/cSim/")
        return
    
    results = {}
    for f in csv_files:
        res = analyze_csv(f)
        if res:
            label = f.stem.replace('flightControlLogs_', '').replace('flightControlLogs', 'default')
            results[label] = res
    
    # Print summary table
    header = f"{'Variant':<20} {'MinDist':>10} {'FinalDist':>10} {'Overshoot':>10} {'Spd@Min':>10} {'1st<500':>8} {'1st<200':>8} {'1st<100':>8} {'Cls/m':>10} {'Div/m':>10}"
    print(header)
    print("-" * len(header))
    
    for label, r in sorted(results.items()):
        print(f"{label:<20} {r['min_dist']:10.2f} {r['final_dist']:10.2f} {r['overshoot']:10.2f} {r['speed_at_min']:10.1f} {str(r['first_500']) or '--':>8} {str(r['first_200']) or '--':>8} {str(r['first_100']) or '--':>8} {r['avg_closing']:10.2f} {r['avg_diverging']:10.2f}")
    
    # Find best
    best = min(results.items(), key=lambda x: x[1]['min_dist'])
    print(f"\nBest min distance: {best[0]} = {best[1]['min_dist']:.2f}m at step {best[1]['min_idx']}")
    
    # Distance over time summary
    print("\n--- Distance milestones ---")
    for label, r in sorted(results.items()):
        milestones = []
        for threshold in [1000, 500, 200, 100, 50]:
            step = next((i for i, d in enumerate(r['distances']) if d < threshold), None)
            if step is not None:
                milestones.append(f"<{threshold}m @ step {step}")
            else:
                milestones.append(f"<{threshold}m: never")
        print(f"{label}: {', '.join(milestones)}")


if __name__ == '__main__':
    main()

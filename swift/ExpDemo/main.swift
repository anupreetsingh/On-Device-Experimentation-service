// Prints this device's variant for each experiment named on the command line.
//   swift run exp-demo new_search_ranking photo_grid_layout
import Experimentation

let experiments = CommandLine.arguments.dropFirst()
if experiments.isEmpty {
    print("usage: exp-demo <experiment>...")
}
for experiment in experiments {
    print("\(experiment): \(Experiments.variant(for: experiment))")
}

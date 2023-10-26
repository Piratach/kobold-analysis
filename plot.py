import argparse
import json
import os
import subprocess
import matplotlib.pyplot as plt

#-------------------------------------------------------------------------
# Parse and generate graphs from comparison.txt
#-------------------------------------------------------------------------

def plot(stat, hmesiStats, koboldStats, percentageDiff, filenamePrefix):
    x_axis = ["hierarchical mesi", "kobold"]
    y_axis = [hmesiStats, koboldStats]

    fig = plt.figure()
    plt.bar(x_axis, y_axis)
    plt.title(stat)
    plt.savefig(filenamePrefix + "_" + stat)

def checkForStats(line, statsToPlot):
    for stats in statsToPlot:
        if stats in line:
            return (True, stats)
    return (False, None)

def parseForValuesOnly(coherenceProtocolName, line):
    line = line.split(coherenceProtocolName + ": ")
    value = (line[1])[:-2] # has a \n at the end of the line
    return float(value)

def parseAndPlot(comparisonFile, statsToPlot, filenamePrefix):
    f = open(comparisonFile, 'r')
    lines = f.readlines()
    for i in range(len(lines)):
        currentLine = lines[i]
        currentLine = currentLine.strip()

        # only process every parameter line
        if not currentLine.startswith("Parameter"):
            continue

        (hasRelevantStats, stat) = \
                checkForStats(currentLine, statsToPlot)
        if not hasRelevantStats:
            continue

        # stats parameter not found, so we ignore it
        if "not found" in currentLine:
          print(currentLine)
          continue

        # from this point on, we know it's a relevant line with stats
        koboldStats = parseForValuesOnly("kobold", lines[i + 1])
        hmesiStats = parseForValuesOnly("hmesi", lines[i + 2])
        percentageDiff = lines[i + 4]
        plot(stat, hmesiStats, koboldStats, percentageDiff, filenamePrefix)

#-------------------------------------------------------------------------
# main
#-------------------------------------------------------------------------

def main():

  # Command-line options
  parser = argparse.ArgumentParser( description='Plot comparison output file' )
  parser.add_argument('-i', '--input-dir',
                       help='Input directory where comparison.txt is located')
  parser.add_argument('-o', '--output-file-prefix',
                       help='Output filename prefix for the graphs produced')
  parser.add_argument('-s', '--statistics', action='append',
          help='Statistics to plot', default=["simSeconds", "L2_misses",
              "L1Cache.miss_mach_latency_hist_seqr::mean"])
  args = parser.parse_args()

  if not os.path.isdir(args.input_dir):
    print("Directory does not exist")
    exit(1)

  comparisonFile  = os.path.join(args.input_dir, "comparison.txt")

  if not os.path.isfile(comparisonFile):
    print("comparison.txt doesn't exist")
    exit(1)

  filePrefix = os.path.join(args.input_dir, args.output_file_prefix)
  statsToPlot = args.statistics

  parseAndPlot(comparisonFile, statsToPlot, filePrefix)

if __name__ == "__main__":
    main()

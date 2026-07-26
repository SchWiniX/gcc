import struct, subprocess, sys
from collections import defaultdict
from pathlib import Path

def read_coverage(bin_path: Path):
	"""Read coverage.bin format: [8B count][8B*count PCs][bitmap]"""
	with open(bin_path, "rb") as f:
		bin_size = struct.unpack("<Q", f.read(8))[0]
		bin = f.read(bin_size)
		total = struct.unpack("<Q", f.read(8))[0]
		pcs = struct.unpack("<" + "Q" * total, f.read(total * 8))
		bitmap = f.read((total + 1) // 8 + 1)
	return bin.decode("utf-8"), total, bitmap, pcs


def symbolize_pc_table(total, pcs, clang_binary) -> dict:
	"""Resolve a PC address to source file using llvm-symbolizer."""
	cmd = [
		"llvm-symbolizer",
		"--obj=" + clang_binary,
		"--pretty-print",
		"--addresses",
		"--relativenames",
		"--no-inlines"
	]

	input = ""
	for idx in range(0, total):
		input += f"0x{pcs[idx]:x}\n"
	result = subprocess.run(
		cmd,
		input=input,
		capture_output=True, text=True
	)
	output = result.stdout

	# Parse llvm-symbolizer output
	pc_map: dict = {}
	for line in output.split('\n'):
		if line.startswith("0x"):
			# Extract file path (simplified)
			address, remainder = line.split(':', 1)
			function, path_and_pos = remainder.split(' at ', 1)
			path, loc = path_and_pos.split(':', 1)
			#pc_map[address] = (function, path, loc)
			pc_map[address] = path

	return pc_map

def analyze_coverage(bin_path):
	clang_binary, total, bitmap, pcs = read_coverage(bin_path)
	assert total > 0, "Total must be larget then 0"
	print(f"[analyzer] Detected binary: {clang_binary}")

	file_stats = defaultdict(lambda: {"hit": 0, "total": 0})

	print(f"[analyzer] Processing PC-Table...")

	pc_map = symbolize_pc_table(total, pcs, clang_binary)

	print(f"[analyzer] Processing {total} edges...")

	for idx in range(1, total + 1):
		file: str = pc_map[f"0x{pcs[idx - 1]:x}"]
		file_stats[file]["total"] += 1
		if bitmap[idx // 8] & (1 << idx % 8) != 0:
			file_stats[file]["hit"] += 1

	# Report results
	print("\n" + "="*60)
	print("Component Coverage Summery")
	print("="*60)
	
	for file, stats in sorted(file_stats.items()):
		if stats["total"] > 0:
			pct = 100.0 * stats["hit"] / stats["total"]
			if file == None:
				print(f"unknown: {pct:6.1f}%  ({stats['hit']:6}/{stats['total']:6} edges)")
			else:
				print(f"{file:128}: {pct:6.1f}%  ({stats['hit']:6}/{stats['total']:6} edges)")
	
	print("="*60)

if __name__ == "__main__":
	if len(sys.argv) < 2:
		print(f"Usage: {sys.argv[0]} <coverage.bin>")
		sys.exit(1)
	analyze_coverage(sys.argv[1])

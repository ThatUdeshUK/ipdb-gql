#!/usr/bin/env python3
"""Validate weighted SSSP against Bellman-Ford and measure cold/warm CLI calls.

Uses only the Python standard library and a locally built DuckDB shell.
No packages, network services, or downloaded datasets are required.
"""
import argparse
import csv
import heapq
import io
import json
import math
import random
import re
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def fixture(directory, name, vertices, edges):
    nodes_path = directory / f"{name}_nodes.csv"
    edges_path = directory / f"{name}_edges.csv"
    with nodes_path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["id:ID", ":LABEL"])
        writer.writerows((i, "node") for i in range(1, vertices + 1))
    with edges_path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([":START_ID", ":END_ID", ":TYPE", "cost:double"])
        writer.writerows(edges)
    return f"""CREATE GRAPH {name} ANY;
COPY GRAPH {name} FROM (VERTICES {quote(nodes_path)}, EDGES {quote(edges_path)}) FORMAT GRAPH;
"""


def arcs(edges, direction, label):
    for edge_id, (src, dst, edge_label, cost) in enumerate(edges, 1):
        if label and label != edge_label:
            continue
        if direction != "in":
            yield src, dst, cost, edge_id
        if direction != "out":
            yield dst, src, cost, edge_id


def bellman_ford(vertices, edges, source, direction, label):
    distances = {v: math.inf for v in range(1, vertices + 1)}
    distances[source] = 0.0
    selected = list(arcs(edges, direction, label))
    for _ in range(vertices - 1):
        changed = False
        for src, dst, cost, _ in selected:
            candidate = distances[src] + cost
            if candidate < distances[dst]:
                distances[dst] = candidate
                changed = True
        if not changed:
            break
    return {v: d for v, d in distances.items() if math.isfinite(d)}, selected


def run(shell, extension, sql):
    result = subprocess.run(
        [str(shell), "-unsigned", "-batch", "-bail", "-csv", "-noheader"],
        input=f"LOAD {quote(extension)};\n" + sql,
        text=True, capture_output=True, check=True,
    )
    return result.stdout


def validate(shell, extension, directory):
    rng = random.Random(97822)
    sql = []
    cases = {}
    for graph_index in range(6):
        count = 40
        name = f"weighted_reference_{graph_index}"
        # Include disconnected vertices, zero weights, loops, parallel edges,
        # dense and sparse shapes, and many decrease-key operations.
        active = count - graph_index
        edges = [(rng.randint(1, active), rng.randint(1, active),
                  rng.choice(["route", "other"]), rng.randint(0, 80) / 4)
                 for _ in range(30 + graph_index * 90)]
        sql += [".output /dev/null\n", fixture(directory, name, count, edges), ".output\n"]
        for source in (1, count):
            for direction in ("out", "in", "both"):
                for label in ("", "route"):
                    key = f"{name}_{source}_{direction}_{label}"
                    expected, selected = bellman_ford(count, edges, source, direction, label)
                    cases[key] = (source, expected, selected)
                    sql.append(f"SELECT {quote(key)}, * FROM system.algo.weighted_sssp("
                               f"{quote(name)}, {source}, 'cost', direction := {quote(direction)}, "
                               f"edge_label := {quote(label)});\n")
    output = run(shell, extension, "".join(sql))
    (directory / "reference-results.csv").write_text(output)
    actual = {key: {} for key in cases}
    for key, vertex, distance, parent, edge, order in csv.reader(io.StringIO(output)):
        vertex = int(vertex)
        assert vertex not in actual[key], (key, "duplicate vertex", vertex)
        actual[key][vertex] = (float(distance), int(parent) if parent not in ("", "NULL") else None,
                               int(edge) if edge not in ("", "NULL") else None, int(order))
    rows = 0
    for key, (source, expected, selected) in cases.items():
        found = actual[key]
        assert {v: row[0] for v, row in found.items()} == expected, (key, found, expected)
        allowed = {(src, dst, edge): cost for src, dst, cost, edge in selected}
        settled = sorted(found, key=lambda v: found[v][3])
        assert [found[v][3] for v in settled] == list(range(len(found))), key
        assert [found[v][0] for v in settled] == sorted(expected.values()), key
        for vertex, (distance, parent, edge, order) in found.items():
            if vertex == source:
                assert (distance, parent, edge, order) == (0, None, None, 0), key
            else:
                assert found[parent][0] + allowed[parent, vertex, edge] == distance, key
                assert found[parent][3] < order, (key, "parent cycle")
        rows += len(found)
    return {"cases": len(cases), "verified_rows": rows, "reference": "Bellman-Ford"}


def benchmark(shell, extension, directory, count):
    rng = random.Random(20260905)
    records = []
    for shape in ("chain", "hub", "random"):
        name = f"weighted_bench_{shape}"
        edges = [(i, i + 1, "route", 0.25) for i in range(1, count)]
        if shape == "hub":
            edges += [(1, i, "route", float(count - i + 1)) for i in range(2, count + 1)]
        elif shape == "random":
            edges += [(rng.randint(1, count), rng.randint(1, count), "route", rng.randint(0, 100) / 4)
                      for _ in range(count * 7)]
        # Independent Python heap implementation checks benchmark results too.
        adjacency = [[] for _ in range(count + 1)]
        for src, dst, _, weight in edges:
            adjacency[src].append((dst, weight))
        distances = [math.inf] * (count + 1)
        distances[1] = 0
        queue = [(0, 1)]
        while queue:
            distance, vertex = heapq.heappop(queue)
            if distance != distances[vertex]:
                continue
            for neighbor, weight in adjacency[vertex]:
                candidate = distance + weight
                if candidate < distances[neighbor]:
                    distances[neighbor] = candidate
                    heapq.heappush(queue, (candidate, neighbor))
        sql = ".output /dev/null\n" + fixture(directory, name, count, edges) + ".output\n.timer on\n"
        for trial in range(4):
            sql += (f"SELECT 'trial', {trial}, count(*), sum(distance) FROM "
                    f"system.algo.weighted_sssp('{name}', 1, 'cost');\n")
        sql += (f".timer off\nSELECT 'memory', memory_bytes, build_count FROM "
                f"gql_csr_stats('{name}', weight_property := 'cost');\n")
        output = run(shell, extension, sql)
        (directory / f"{shape}-timings.txt").write_text(output)
        timings = []
        trials = 0
        memory_bytes = None
        for line in output.splitlines():
            if line.startswith("Run Time"):
                timings.append(float(re.search(r"real ([0-9.]+)", line)[1]) * 1000)
            elif line.startswith("trial,"):
                _, _, reached, total = next(csv.reader([line]))
                assert int(reached) == count and float(total) == sum(distances[1:]), shape
                trials += 1
            elif line.startswith("memory,"):
                _, memory_bytes, builds = next(csv.reader([line]))
                assert int(builds) == 1, (shape, "warm query rebuilt CSR")
        assert trials == len(timings) == 4 and memory_bytes is not None, output
        records.append({"shape": shape, "vertices": count, "edges": len(edges),
                        "cold_ms": timings[0], "warm_ms": timings[1:],
                        "warm_median_ms": statistics.median(timings[1:]),
                        "csr_memory_bytes": int(memory_bytes)})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell", type=Path, default=ROOT / "build/release/duckdb")
    parser.add_argument("--extension", type=Path,
                        default=ROOT / "build/release/extension/duckgql/duckgql.duckdb_extension")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vertices", type=int, default=10000)
    args = parser.parse_args()
    if args.vertices < 2:
        parser.error("--vertices must be at least 2")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"correctness": validate(args.shell, args.extension, args.output),
              "benchmarks": benchmark(args.shell, args.extension, args.output, args.vertices),
              "measurement": "CLI wall time, 1 ms precision; cold includes CSR build; warm includes weight validation; "
                             "CSR bytes exclude traversal buffers and DuckDB process memory"}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

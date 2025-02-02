/*
 * Copyright (c) 2023 Gabriel Gouvine
 */

#include "kernel/celltypes.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"
#include "kernel/yosys.h"

#include "gate_insertion.hpp"
#include "logic_locking_analyzer.hpp"
#include "logic_locking_optimizer.hpp"
#include "mini_aig.hpp"
#include "output_corruption_optimizer.hpp"

#include <random>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

enum OptimizationTarget { PAIRWISE_SECURITY, OUTPUT_CORRUPTION, HYBRID };

LogicLockingOptimizer make_optimizer(const std::vector<Cell *> &cells, const std::vector<std::pair<Cell *, Cell *>> &pairwise_security)
{
	pool<Cell *> cell_set(cells.begin(), cells.end());
	for (auto p : pairwise_security) {
		assert(cell_set.count(p.first));
		assert(cell_set.count(p.second));
	}
	dict<Cell *, int> cell_to_ind;
	for (int i = 0; i < GetSize(cells); ++i) {
		cell_to_ind[cells[i]] = i;
	}

	std::vector<std::vector<int>> gr(cells.size());
	for (auto p : pairwise_security) {
		int i = cell_to_ind[p.first];
		int j = cell_to_ind[p.second];
		gr[i].push_back(j);
		gr[j].push_back(i);
	}

	return LogicLockingOptimizer(gr);
}

OutputCorruptionOptimizer make_optimizer(const std::vector<Cell *> &cells, const dict<Cell *, std::vector<std::vector<std::uint64_t>>> &data)
{
	std::vector<std::vector<std::uint64_t>> corruptionData;
	for (Cell *c : cells) {
		std::vector<std::uint64_t> cellCorruption;
		for (const auto &v : data.at(c)) {
			for (std::uint64_t d : v) {
				cellCorruption.push_back(d);
			}
		}
		corruptionData.push_back(cellCorruption);
	}
	return OutputCorruptionOptimizer(corruptionData);
}

std::vector<Cell *> optimize_pairwise_security(const std::vector<Cell *> &cells, const std::vector<std::pair<Cell *, Cell *>> &pairwise_security,
					       int maxNumber)
{
	auto opt = make_optimizer(cells, pairwise_security);

	log("Running optimization on the interference graph with %d non-trivial nodes out of %d and %d edges.\n", opt.nbConnectedNodes(),
	    opt.nbNodes(), opt.nbEdges());
	auto sol = opt.solveGreedy(maxNumber);

	std::vector<Cell *> ret;
	for (const auto &clique : sol) {
		for (int c : clique) {
			ret.push_back(cells[c]);
		}
	}

	double security = opt.value(sol);
	log("Locking solution with %d cliques, %d locked wires and %.2f estimated security.\n", (int)sol.size(), (int)ret.size(), security);
	return ret;
}

std::vector<Cell *> optimize_output_corruption(const std::vector<Cell *> &cells, const dict<Cell *, std::vector<std::vector<std::uint64_t>>> &data,
					       int maxNumber)
{
	auto opt = make_optimizer(cells, data);

	log("Running corruption optimization with %d unique nodes out of %d.\n", (int)opt.getUniqueNodes().size(), opt.nbNodes());
	std::vector<int> sol = opt.solveGreedy(maxNumber, std::vector<int>());
	float cover = 100.0 * opt.corruptionCover(sol);
	float rate = 100.0 * opt.corruptionRate(sol);

	log("Locking solution with %d locked wires, %.2f%% corruption cover and %.2f%% corruption rate.\n", (int)sol.size(), cover, rate);

	std::vector<Cell *> ret;
	for (int c : sol) {
		ret.push_back(cells[c]);
	}
	return ret;
}

std::vector<Cell *> optimize_hybrid(const std::vector<Cell *> &cells, const std::vector<std::pair<Cell *, Cell *>> &pairwise_security,
				    const dict<Cell *, std::vector<std::vector<std::uint64_t>>> &data, int maxNumber)
{
	auto pairw = make_optimizer(cells, pairwise_security);
	auto corr = make_optimizer(cells, data);

	log("Running hybrid optimization\n");
	log("Interference graph with %d non-trivial nodes out of %d and %d edges.\n", pairw.nbConnectedNodes(), pairw.nbNodes(), pairw.nbEdges());
	log("Corruption data with %d unique nodes out of %d.\n", (int)corr.getUniqueNodes().size(), corr.nbNodes());
	auto pairwSol = pairw.solveGreedy(maxNumber);
	std::vector<int> largestClique;
	if (!pairwSol.empty() && pairwSol.front().size() > 1) {
		largestClique = pairwSol.front();
	}

	std::vector<int> sol = corr.solveGreedy(maxNumber, largestClique);
	float cover = 100.0 * corr.corruptionCover(sol);
	float rate = 100.0 * corr.corruptionRate(sol);

	log("Locking solution with %d locked wires, largest clique of size %d, %.2f%% corruption cover and %.2f%% corruption rate.\n",
	    (int)sol.size(), (int)largestClique.size(), cover, rate);

	std::vector<Cell *> ret;
	for (int c : sol) {
		ret.push_back(cells[c]);
	}
	return ret;
}

void report_tradeoff(const std::vector<Cell *> &cells, const dict<Cell *, std::vector<std::vector<std::uint64_t>>> &data)
{
	log("Reporting output corruption by number of cells locked\n");
	auto opt = make_optimizer(cells, data);
	auto order = opt.solveGreedy(opt.nbNodes(), std::vector<int>());
	log("Locked\tCover\tRate\n");
	for (int i = 1; i <= GetSize(order); ++i) {
		std::vector<int> sol = order;
		sol.resize(i);
		double cover = 100.0 * opt.corruptionCover(sol);
		double rate = 100.0 * opt.corruptionRate(sol);
		log("%d\t%.2f\t%.2f\n", i, cover, rate);
	}
	log("\n\n");
}

void report_tradeoff(const std::vector<Cell *> &cells, const std::vector<std::pair<Cell *, Cell *>> &pairwise_security)
{
	log("Reporting pairwise security by number of cells locked\n");
	auto opt = make_optimizer(cells, pairwise_security);
	auto all_cliques = opt.solveGreedy(opt.nbNodes());
	log("Locked\tSecurity\n");
	int nbLocked = 0;
	for (int i = 0; i < GetSize(all_cliques); ++i) {
		std::vector<int> clique = all_cliques[i];
		for (int j = 1; j <= GetSize(clique); ++j) {
			std::vector<std::vector<int>> sol = all_cliques;
			sol.resize(i + 1);
			sol.back().resize(j);
			double security = opt.value(sol);
			log("%d\t%.2f\n", nbLocked + j, security);
		}
		nbLocked += GetSize(clique);
	}
	log("\n\n");
}

void report_logic_locking(RTLIL::Module *module, int nb_test_vectors)
{
	LogicLockingAnalyzer pw(module);
	pw.gen_test_vectors(nb_test_vectors, 1);

	std::vector<Cell *> lockable_cells = pw.get_lockable_cells();
	auto corruption_data = pw.compute_output_corruption_data();
	auto pairwise_security = pw.compute_pairwise_secure_graph();
	report_tradeoff(lockable_cells, corruption_data);
	report_tradeoff(lockable_cells, pairwise_security);
}

std::vector<Cell *> run_logic_locking(RTLIL::Module *module, int nb_test_vectors, int nb_locked, OptimizationTarget target)
{
	LogicLockingAnalyzer pw(module);
	pw.gen_test_vectors(nb_test_vectors, 1);

	std::vector<Cell *> lockable_cells = pw.get_lockable_cells();
	std::vector<Cell *> locked_gates;
	if (target == PAIRWISE_SECURITY) {
		auto pairwise_security = pw.compute_pairwise_secure_graph();
		locked_gates = optimize_pairwise_security(lockable_cells, pairwise_security, nb_locked);
	} else if (target == OUTPUT_CORRUPTION) {
		auto corruption_data = pw.compute_output_corruption_data();
		locked_gates = optimize_output_corruption(lockable_cells, corruption_data, nb_locked);
	} else if (target == HYBRID) {
		auto pairwise_security = pw.compute_pairwise_secure_graph();
		auto corruption_data = pw.compute_output_corruption_data();
		locked_gates = optimize_hybrid(lockable_cells, pairwise_security, corruption_data, nb_locked);
	}
	return locked_gates;
}

/**
 * @brief Parse a boolean value
 */
static bool parse_bool(const std::string &str)
{
	if (str == "0" || str == "false") {
		return false;
	}
	if (str == "1" || str == "true") {
		return true;
	}
	log_error("Invalid boolean value: %s", str.c_str());
}

/**
 * Create a locking key
 */
static std::vector<bool> create_key(int nb_locked)
{
	std::vector<bool> key_values;
	std::random_device rgen;
	std::bernoulli_distribution dist;
	for (int i = 0; i < nb_locked; ++i) {
		key_values.push_back(dist(rgen));
	}
	return key_values;
}

/**
 * Parse an hexadecimal string
 */
static std::vector<bool> parse_hex_string(const std::string &str)
{
	std::vector<bool> ret;
	for (auto it = str.rbegin(); it != str.rend(); ++it) {
		char cur = *it;
		char c = std::tolower(cur);
		int v = 0;
		if (c >= '0' && c <= '9') {
			v = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			v = (c - 'a') + 10;
		} else {
			log_error("<%c> is not a proper hexadecimal character\n", cur);
		}
		for (int i = 0; i < 4; ++i) {
			ret.push_back(v % 2);
			v /= 2;
		}
	}
	return ret;
}

static std::string create_hex_string(std::vector<bool> &key)
{
	std::string ret;
	for (int i = 0; i < GetSize(key); i += 4) {
		int v = 0;
		int c = 1;
		for (int j = i; j < i + 4 && j < GetSize(key); ++j) {
			if (key[j]) {
				v += c;
			}
			c *= 2;
		}
		if (v < 10) {
			ret.push_back('0' + v);
		} else {
			ret.push_back('a' + (v - 10));
		}
	}
	std::reverse(ret.begin(), ret.end());
	return ret;
}

struct LogicLockingPass : public Pass {
	LogicLockingPass() : Pass("logic_locking") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing LOGIC_LOCKING pass.\n");
		OptimizationTarget target = PAIRWISE_SECURITY;
		double percent_locked = 5.0f;
		int key_size = -1;
		int nb_test_vectors = 64;
		bool report = false;
		std::vector<IdString> gates_to_lock;
		std::string key;
		std::vector<std::pair<IdString, IdString>> gates_to_mix;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			if (arg == "-lock-gate") {
				if (argidx + 1 >= args.size())
					break;
				gates_to_lock.emplace_back(args[++argidx]);
				continue;
			}
			if (arg == "-mix-gate") {
				if (argidx + 2 >= args.size())
					break;
				IdString n1 = args[++argidx];
				IdString n2 = args[++argidx];
				gates_to_mix.emplace_back(n1, n2);
				continue;
			}
			if (arg == "-key-percent") {
				if (argidx + 1 >= args.size())
					break;
				percent_locked = std::atof(args[++argidx].c_str());
				continue;
			}
			if (arg == "-key-bits") {
				if (argidx + 1 >= args.size())
					break;
				key_size = std::atoi(args[++argidx].c_str());
				continue;
			}
			if (arg == "-nb-test-vectors") {
				if (argidx + 1 >= args.size())
					break;
				nb_test_vectors = std::atoi(args[++argidx].c_str());
				continue;
			}
			if (arg == "-target") {
				if (argidx + 1 >= args.size())
					break;
				auto t = args[++argidx];
				if (t == "pairwise") {
					target = PAIRWISE_SECURITY;
				} else if (t == "corruption") {
					target = OUTPUT_CORRUPTION;
				} else if (t == "hybrid") {
					target = HYBRID;
				} else {
					log_error("Invalid target option %s", t.c_str());
				}
				continue;
			}
			if (arg == "-key") {
				if (argidx + 1 >= args.size())
					break;
				key = args[++argidx];
				continue;
			}
			if (arg == "-report") {
				report = true;
				continue;
			}
			break;
		}

		log_assert(percent_locked >= 0.0f);
		log_assert(percent_locked <= 100.0f);

		// handle extra options (e.g. selection)
		extra_args(args, argidx, design);

		std::vector<RTLIL::Module *> modules_to_run;
		for (auto &it : design->modules_) {
			if (design->selected_module(it.first)) {
				modules_to_run.push_back(it.second);
			}
		}
		if (modules_to_run.size() >= 2) {
			log_error("Multiple modules are selected. Please run logic locking on a single module to avoid duplicate keys.\n");
		}
		if (modules_to_run.empty()) {
			return;
		}

		RTLIL::Module *mod = modules_to_run.front();

		bool explicit_locking = !gates_to_lock.empty() || !gates_to_mix.empty();
		int nb_locked;
		if (explicit_locking) {
			nb_locked = gates_to_lock.size() + gates_to_mix.size();
		} else if (key_size >= 0) {
			nb_locked = key_size;
		} else {
			nb_locked = static_cast<int>(0.01 * GetSize(mod->cells_) * percent_locked);
		}

		std::vector<bool> key_values;
		if (key.empty()) {
			key_values = create_key(nb_locked);
		} else {
			key_values = parse_hex_string(key);
		}
		if (nb_locked > GetSize(key_values)) {
			log_error("Key size is %d bits, which is not enough to lock %d gates\n", GetSize(key_values), nb_locked);
		}
		std::string key_check = create_hex_string(key_values);

		/*
		 * TODO: the locking should be at the signal level, not the gate level.
		 * This would allow locking on the input ports.
		 *
		 * At the moment, the locking gate is added right after the cell, replacing
		 * its original output wire.
		 * To implement locking on input ports, we need to lock after the input instead,
		 * so that the name is kept, and update all reader cells.
		 * This would give more targets for locking, as primary inputs are not considered
		 * right now.
		 */
		if (explicit_locking) {
			log("Explicit logic locking solution: %zu xor locks and %zu mux locks, key %s\n", gates_to_lock.size(), gates_to_mix.size(),
			    key_check.c_str());
			RTLIL::Wire *w = add_key_input(mod, nb_locked);
			int nb_xor_gates = gates_to_lock.size();
			std::vector<bool> lock_key(key_values.begin(), key_values.begin() + nb_xor_gates);
			lock_gates(mod, gates_to_lock, SigSpec(w, 0, nb_xor_gates), lock_key);
			std::vector<bool> mix_key(key_values.begin() + gates_to_lock.size(), key_values.begin() + nb_locked);
			mix_gates(mod, gates_to_mix, SigSpec(w, nb_xor_gates, nb_locked), mix_key);
			return;
		} else if (report) {
			report_logic_locking(mod, nb_test_vectors);
		} else {
			log("Running logic locking with %d test vectors, locking %d cells out of %d, key %s.\n", nb_test_vectors, nb_locked,
			    GetSize(mod->cells_), key_check.c_str());
			auto locked_gates = run_logic_locking(mod, nb_test_vectors, nb_locked, target);
			nb_locked = locked_gates.size();
			RTLIL::Wire *w = add_key_input(mod, nb_locked);
			key_values.erase(key_values.begin() + nb_locked, key_values.end());
			lock_gates(mod, locked_gates, SigSpec(w), key_values);
		}
	}

	void help() override
	{
		log("\n");
		log("    logic_locking [options]\n");
		log("\n");
		log("This command adds inputs to the design, so that a secret value \n");
		log("is required to obtain the correct functionality.\n");
		log("By default, it runs simulations and optimizes the subset of signals that \n");
		log("are locked, making it difficult to recover the original design.\n");
		log("\n");
		log("    -key <value>\n");
		log("        the locking key (hexadecimal)\n");
		log("\n");
		log("    -key-bits <value>\n");
		log("        specify the size of the key in bits\n");
		log("\n");
		log("    -key-percent <value>\n");
		log("        specify the size of the key as a percentage of the number of gates in the design (default=5)\n");
		log("\n");
		log("    -target {pairwise|corruption|hybrid}\n");
		log("        specify the optimization target for locking (default=pairwise)\n");
		log("\n");
		log("    -nb-test-vectors <value>\n");
		log("        specify the number of test vectors used for analysis (default=64)\n");
		log("\n");
		log("    -report\n");
		log("        print statistics but do not modify the circuit\n");
		log("\n");
		log("\n");
		log("The following options control locking manually, locking the corresponding \n");
		log("gate outputs directly without any optimization. They can be mixed and repeated.\n");
		log("\n");
		log("    -lock-gate <name>\n");
		log("        lock the output of the gate, adding a xor/xnor and a module input.\n");
		log("\n");
		log("    -mix-gate <name1> <name2>\n");
		log("        mix the output of one gate with another, adding a mux and a module input.\n");
		log("\n");
		log("\n");
		log("Security is evaluated with simple metrics:\n");
		log("  * Target \"corruption\" maximizes the impact of the locked signals on the outputs.\n");
		log("It will chose signals that cause changes in as many outputs for as many \n");
		log("test vectors as possible.\n");
		log("  * Target \"pairwise\" maximizes the number of mutually pairwise-secure signals.\n");
		log("Two signals are pairwise secure if the value of the locking key for one of them \n");
		log("cannot be recovered just by controlling the inputs, independently of the other.\n");
		log("Additionally, the MOOSIC plugin forces \"useful\" pairwise security, which \n");
		log("prevents redundant locking in buffer chains or xor trees.\n");
		log("\n");
		log("Only gate outputs (not primary inputs) are considered for locking at the moment.\n");
		log("Sequential cells and hierarchical instances are treated as primary inputs and outputs \n");
		log("for security evaluation.\n");
		log("\n");
		log("\n");
	}

} LogicLockingPass;

PRIVATE_NAMESPACE_END

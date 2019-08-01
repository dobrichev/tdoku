#include <algorithm>
#include <array>
#include <bitset>
#include <climits>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <vector>

using namespace std;

namespace {

constexpr int kNumBoxes = 9;
constexpr int kNumPosClausesPerBox = 16; // 9 cells, 6 triads, 1 slack
constexpr int kNumValues = 9;

constexpr uint16_t kNumLiterals = kNumBoxes * kNumPosClausesPerBox * kNumValues * 2;
constexpr uint16_t kAllAssigned = kNumBoxes * (kNumPosClausesPerBox - 1) * kNumValues;

typedef uint32_t ClauseId;
typedef uint32_t LiteralId;

struct State {
    // 1s for asserted literals, 0s for literals negated or unknown
    bitset<kNumLiterals> truth_value;
    // the number of literals that can be eliminated before the clause produces binary implications
    vector<uint16_t> clause_free_literals;
    // the number of implications for a given literal. we will not copy the implication lists
    // themselves as part of the state. instead the global state has a vector for each literal
    // that we use as a stack, and these counts are the stack pointers.
    array<uint16_t, kNumLiterals> implication_counts;
    // number of literals assigned. we are done when this equals kAllAssigned.
    uint32_t num_assigned = 0;

    State() : truth_value{}, clause_free_literals{}, implication_counts{} {}
    State(const State &prior_state) = default;
};

struct SolverDpllTriadScc {
    // this mapping from ClauseId to LiteralId will not change after setup.
    vector<vector<LiteralId>> clauses_to_literals_{};
    // this mapping from LiteralId to ClauseId will not change after setup.
    array<vector<ClauseId>, kNumLiterals> literals_to_clauses_{};
    // initial setup will populate these vectors with implications that are part of Sudoku
    // rules. during BCP and DPLL search we'll discover new implications and push and pop them
    // from these vectors. we don't copy these vectors as part of the state. instead we just
    // copy implication counts that determine the logical size of these lists.
    array<vector<LiteralId>, kNumLiterals> literals_to_implications_{};
    // a list of clauses expressing that each cell must have a value. if we're not using SCCs
    // for choosing literals to branch then it suffices to pick among these clauses and then
    // pick a literal from the chosen clause.
    vector<ClauseId> positive_cell_clauses_{};
    // initial state with the correct implication counts. we'll clone this when we begin
    // solving each new puzzle, but this will not change after setup.
    State clean_slate_{};
    // whether to use strongly connected component size as a heuristic for variable selection.
    bool scc_heuristic_ = true;
    // whether to apply inferences reached during strongly connected component evaluation.
    bool scc_inference_ = true;
    // stop after finding this many solutions.
    size_t limit_ = 1;

    size_t num_guesses_ = 0;
    size_t num_solutions_ = 0;
    State result_{};

    SolverDpllTriadScc() {
        SetupConstraints();
    }

    static void Display(State *state) {
        string div1 = " +=====+=====+=====+=====+=====+=====+=====+=====+=====+=====+=====+=====+";
        string div2 = " +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+";
        for (int i = 0; i < 12; i++) {
            cout << (((i % 4) == 0) ? div1 : div2) << endl;
            for (int vi = 0; vi < 3; vi++) {
                for (int j = 0; j < 12; j++) {
                    cout << " | ";
                    for (int vj = 0; vj < 3; vj++) {
                        int box = i / 4 * 3 + j / 4;
                        int elm = (i % 4) * 4 + (j % 4);
                        if (state->truth_value[Not(Lit(box, elm, vi * 3 + vj))]) {
                            cout << " ";
                        } else {
                            cout << vi * 3 + vj + 1;
                        }
                    }
                }
                cout << " |" << endl;
            }
        }
        cout << div1 << endl << endl;
    }

    ///////////////////////////////////////////////
    // constraint setup
    ///////////////////////////////////////////////

    // literals are numbered so positive and negative literals for the same variable are adjacent.
    // a positive literal has id % 2 == 0.
    static inline LiteralId Not(LiteralId literal) {
        return literal ^ 1u;
    }

    // returns a *positive* literal id reflecting the proposition that the given element of the
    // given box has the given value. boxes and values are numbered 0-8. elements are numbered
    // based on a 4x4 grid, with the upper-left 3x3 subgrid being the actual 9 cells of the box
    // and the 3x1 and 1x3 extra column and row being horizontal and vertical triads. The last
    // element of the 4x4 grid is unused, but remains for indexing convenience.
    static uint32_t Lit(int box, int elem, int value) {
        return 2 * (value + 9 * (elem + 16 * box));
    }

    // return true if the literal is in use (vs. in the filler space at the end of each box).
    static bool ValidLiteral(LiteralId literal) {
        return ((literal / 18) % 16) != 15;
    }

    inline void AddImplication(LiteralId from, LiteralId to, State *state) {
        auto &implications = literals_to_implications_[from];
        auto &current_size = state->implication_counts[from];
        if (implications.size() == current_size) {
            implications.push_back(to);
        } else {
            implications[current_size] = to;
        }
        current_size++;
    }

    inline void AddClauseWithMinimum(const vector<LiteralId> &literals, int min) {
        ClauseId new_clause_id = clauses_to_literals_.size();
        for (LiteralId literal : literals) {
            literals_to_clauses_[literal].push_back(new_clause_id);
        }
        clauses_to_literals_.push_back(literals);
        clean_slate_.clause_free_literals.push_back(literals.size() - 1 - min);
        if (min == 1 && literals.size() == 9) {
            positive_cell_clauses_.push_back(new_clause_id);
        }
    }

    void AddExactlyNConstraint(const vector<LiteralId> &literals, int n) {
        AddClauseWithMinimum(literals, n);
        if (n == 1) {
            for (size_t i = 0; i < literals.size() - 1; i++) {
                for (size_t j = i + 1; j < literals.size(); j++) {
                    AddImplication(literals[i], Not(literals[j]), &clean_slate_);
                    AddImplication(literals[j], Not(literals[i]), &clean_slate_);
                }
            }
        } else {
            vector<LiteralId> negations;
            negations.reserve(literals.size());
            for (auto literal : literals) negations.push_back(Not(literal));
            AddClauseWithMinimum(negations, negations.size() - n);
        }
    }

    void SetupConstraints() {
        for (int box = 0; box < 9; box++) {
            // ExactlyN constraints over values for a given cell or triad [1/9] and [3/9]
            for (int elem = 0; elem < 15; elem++) {
                vector<LiteralId> literals;
                for (int val = 0; val < 9; val++) {
                    literals.push_back(Lit(box, elem, val));
                }
                // exactly one for normal cells, exactly three for triads
                if (elem / 4 < 3 && elem % 4 < 3) {
                    AddExactlyNConstraint(literals, 1);
                } else {
                    AddExactlyNConstraint(literals, 3);
                }
            }
            // ExactlyN constraints to define each triad [1/4]
            for (int val = 0; val < 9; val++) {
                for (int i = 0; i < 3; i++) {
                    vector<LiteralId> h_triad, v_triad;
                    for (int j = 0; j < 3; j++) {
                        h_triad.push_back(Lit(box, i * 4 + j, val));
                        v_triad.push_back(Lit(box, i + j * 4, val));
                    }
                    h_triad.push_back(Not(Lit(box, i * 4 + 3, val)));
                    v_triad.push_back(Not(Lit(box, i + 12, val)));
                    AddExactlyNConstraint(h_triad, 1);
                    AddExactlyNConstraint(v_triad, 1);
                }
            }
        }
        // ExactlyN constraints over band triads within and across boxes [1/3]
        for (int val = 0; val < 9; val++) {
            for (int band = 0; band < 3; band++) {
                for (int i = 0; i < 3; i++) {
                    vector<LiteralId> h_within, h_across, v_within, v_across;
                    for (int j = 0; j < 3; j++) {
                        h_within.push_back(Lit(band * 3 + i, j * 4 + 3, val));
                        h_across.push_back(Lit(band * 3 + j, i * 4 + 3, val));
                        v_within.push_back(Lit(i * 3 + band, j + 12, val));
                        v_across.push_back(Lit(j * 3 + band, i + 12, val));
                    }
                    AddExactlyNConstraint(h_within, 1);
                    AddExactlyNConstraint(h_across, 1);
                    AddExactlyNConstraint(v_within, 1);
                    AddExactlyNConstraint(v_across, 1);
                }
            }
        }
    }

    ///////////////////////////////////////////////
    // boolean constraint propagation
    ///////////////////////////////////////////////

    // we have a clause with a minimum of N that's now down to N+1 literals. if any of its
    // remaining literals are eliminated then the rest are implied.
    void AddBinaryImplicationsAmongNonEliminated(ClauseId clause_id, State *state) {
        vector<LiteralId> noneliminated;
        for (LiteralId literal : clauses_to_literals_[clause_id]) {
            if (!state->truth_value[Not(literal)]) {
                noneliminated.push_back(literal);
            }
        }
        // add all pairwise implications
        for (size_t i = 0; i < noneliminated.size() - 1; i++) {
            for (size_t j = i + 1; j < noneliminated.size(); j++) {
                AddImplication(Not(noneliminated[i]), noneliminated[j], state);
                AddImplication(Not(noneliminated[j]), noneliminated[i], state);
            }
        }
    }

    bool Assign(LiteralId literal, State *state) {
        if (state->truth_value[literal]) {
            return true;
        }
        if (state->truth_value[Not(literal)]) {
            return false;
        }
        state->truth_value[literal] = true;
        state->num_assigned++;

        // decrement free literal counts for clauses containing the negation to reflect that these
        // literals are eliminated; update implication lists if this produces new binary clauses.
        for (auto clause_id : literals_to_clauses_[Not(literal)]) {
            if (--state->clause_free_literals[clause_id] ==  0) {
                AddBinaryImplicationsAmongNonEliminated(clause_id, state);
            }
        }
        // now perform unit propagation by assigning implications of this literal
        // note: new implications can be added during this loop.
        vector<LiteralId> &implications = literals_to_implications_[literal];
        uint16_t &num_implications = state->implication_counts[literal];
        for (int i = 0; i < num_implications; i++) {
            if (!Assign(implications[i], state)) return false;
        }
        return true;
    }

    ///////////////////////////////////////////////
    // path-based strongly connected components
    ///////////////////////////////////////////////

    int preorder_counter = 0;
    array<int, kNumLiterals> preorder_index{};
    vector<uint16_t> stack_p;
    vector<uint16_t> stack_s;
    vector<uint16_t> check_vec{};

    array<int, kNumLiterals> literal_to_component_id{};
    vector<uint16_t> component_id_to_component_size;
    int next_component_id = 0;

    // returns false if visitation finds us to be in an inconsistent state.
    bool SccVisit(LiteralId literal, State *state) {
        preorder_index[literal] = preorder_counter++;
        if (scc_inference_ && !state->truth_value[literal] &&
            find(stack_s.begin(), stack_s.end(), Not(literal)) != stack_s.end()) {
            // if a literal is implied by its negation then it can be asserted immediately.
            // since the literal is now asserted anything in its implication list is already
            // asserted and the corresponding binary clauses subsumed. we should proceed as if
            // they don't exist and return without visiting children, without creating a
            // single-member component, and considering the implication count of this branch zero.
            return Assign(literal, state);
        }
        stack_p.push_back(literal);
        stack_s.push_back(literal);

        auto &implications = literals_to_implications_[literal];
        auto &num_implications = state->implication_counts[literal];
        for (size_t i = 0; i < num_implications; i++) {
            uint16_t implication = implications[i];
            if (state->truth_value[implication]) {
                // we can skip any already-asserted implications. these correspond to subsumed
                // binary clauses that have no effect on inference.
                continue;
            }
            if (scc_inference_ && state->truth_value[Not(implication)]) {
                // when we're not apply inferences discovered during scc evaluation we should
                // never visit negated literals since a chain of modus tollens should have negated
                // the root before we began. however, when we *are* applying inferences we may make
                // an assignment during our visitation that leads to a conflict elsewhere.
                return Assign(Not(stack_p[0]), state);
            }
            if (preorder_index[implication] == -1) {
                if (!SccVisit(implication, state)) {
                    return false;
                }
            } else if (literal_to_component_id[implication] == -1) {
                while (preorder_index[stack_p.back()] > preorder_index[implication]) {
                    stack_p.pop_back();
                }
            }
        }
        if (literal == stack_p.back()) {
            stack_p.pop_back();
            int component_id = next_component_id++;
            int component_size = 0;
            check_vec.clear();
            uint32_t s_back;
            do {
                s_back = stack_s.back();
                stack_s.pop_back();
                // if we have a component containing a literal and its negation then we are in
                // an inconsistent state already (this can arise without triggering the path
                // check we've already done).
                if (scc_inference_ &&
                    find(check_vec.begin(), check_vec.end(), Not(s_back)) != check_vec.end()) {
                    return false;
                }
                check_vec.push_back(s_back);
                literal_to_component_id[s_back] = component_id;
                component_size++;
            } while (s_back != literal);
            component_id_to_component_size.push_back(component_size);
        }
        return true;
    }

    bool FindStronglyConnectedComponents(State *state) {
        preorder_counter = 0;
        preorder_index.fill(-1);
        stack_p.clear();
        stack_s.clear();
        next_component_id = 0;
        literal_to_component_id.fill(-1);
        component_id_to_component_size.clear();
        // if we're only interested in component size we could restrict to positive literals as
        // roots since complementary literals are guaranteed (thanks to exactly-one constraint)
        // to appear in components of the same size. however, one will occur before the other
        // topologically, and this may give us reason to prefer one over the other.
        for (uint16_t literal = 0; literal < kNumLiterals; literal++) {
            if (state->truth_value[literal] || state->truth_value[Not(literal)]) {
                // we want SCCs of the graph of binary clauses, excluding subsumed clauses
                // and clauses that are actually unit due to an asserted negation.
                continue;
            }
            if (preorder_index[literal] == -1) {
                if (!SccVisit(literal, state)) return false;
            }
        }
        return true;
    }

    ///////////////////////////////////////////////
    // heuristic search
    ///////////////////////////////////////////////

    // find a literal in a large component and possibly with favorable topological ordering.
    LiteralId ChooseLiteralToBranchByComponent(State *state) {
        int best_literal = -1;
        double max_weight = -1.0;
        for (int i = 0; i < kNumLiterals; i++) {
            if (ValidLiteral(i) && !state->truth_value[i] && !state->truth_value[Not(i)]) {
                auto size = component_id_to_component_size[literal_to_component_id[i]];
                // first we'll prefer a literal in a component of maximum size, and next we'll
                // prefer a component with a large ID since the component IDs are in reverse
                // topological order. a literal and its negation will be in separate components
                // both of the same size. however, topologically we may prefer one over the other.
                double weight = size + 1.0 - 1.0 / (1 + literal_to_component_id[i]);
                if (weight > max_weight) {
                    max_weight = weight;
                    best_literal = i;
                }
            }
        }
        return best_literal;
    }

    // find a positive clause with as few undetermined literals as possible and return one
    // such literal. assumes that the puzzle is *not* already solved.
    LiteralId ChooseLiteralToBranchByClause(State *state) {
        int min_free = INT8_MAX, which_clause = 0;
        for (int clause_id : positive_cell_clauses_) {
            int num_free = state->clause_free_literals[clause_id];
            if (num_free < min_free) {
                min_free = num_free;
                which_clause = clause_id;
            }
        }
        for (LiteralId literal : clauses_to_literals_[which_clause]) {
            if (!state->truth_value[Not(literal)]) {
                return literal;
            }
        }
        exit(1); // shouldn't be possible if puzzle is unsolved.
    }

    void BranchOnLiteral(LiteralId literal, State *state) {
        num_guesses_++;
        State state_copy = *state;
        if (Assign(literal, &state_copy)) {
            CountSolutionsConsistentWithPartialAssignment(&state_copy);
            if (num_solutions_ == limit_) {
                return;
            }
        }
        if (Assign(Not(literal), state)) {
            CountSolutionsConsistentWithPartialAssignment(state);
        }
    }

    void CountSolutionsConsistentWithPartialAssignment(State *state) {
        if (scc_heuristic_ || scc_inference_) {
            uint32_t assigned;
            do {
                assigned = state->num_assigned;
                if (!FindStronglyConnectedComponents(state)) return;
            } while (assigned != state->num_assigned);
        }
        if (state->num_assigned == kAllAssigned) {
            if (++num_solutions_ == 1) {
                result_ = *state;
            }
            return;
        } else {
            LiteralId branch_literal = scc_heuristic_ ?
                    ChooseLiteralToBranchByComponent(state) :
                    ChooseLiteralToBranchByClause(state);
            BranchOnLiteral(branch_literal, state);
        }
    }

    size_t SolveSudoku(const char *input, size_t limit, uint32_t configuration,
                    char *solution, size_t *num_guesses) {
        limit_ = limit;
        scc_inference_ = (configuration & 1u) > 0;
        scc_heuristic_ = (configuration & 2u) > 0;
        num_solutions_ = 0;
        num_guesses_ = 0;

        result_ = clean_slate_;
        State state = clean_slate_;

        for (int i = 0; i < 81; i++) {
            char digit = input[i];
            if (digit != '.') {
                int box = i / 27 * 3 + (i % 9) / 3;
                int elm = ((i / 9) % 3) * 4 + (i % 3);
                int val = digit - '1';
                if (!Assign(Lit(box, elm, val), &state)) return 0;
            }
        }
        CountSolutionsConsistentWithPartialAssignment(&state);

        for (int i = 0; i < 81; i++) {
            int box = i / 27 * 3 + (i % 9) / 3;
            int elm = ((i / 9) % 3) * 4 + (i % 3);
            for (int val = 0; val < 9; val++) {
                if (result_.truth_value[Lit(box, elm, val)]) {
                    solution[i] = char('1' + val);
                }
            }
        }
        *num_guesses = num_guesses_;
        return num_solutions_;
    }
};

}  // namespace


extern "C"
size_t TdokuSolverDpllTriadScc(const char *input, size_t limit, uint32_t configuration,
                               char *solution, size_t *num_guesses) {
    static SolverDpllTriadScc solver;
    return solver.SolveSudoku(input, limit, configuration, solution, num_guesses);
}

#include "agent.h"
#include "game.h"
#include "avl.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

// Constantes
#define MAX_MOVES  ATAXX_MAX_MOVES
#define INF        1000000

// Table de transposition (AVL)
static int tt_encode(int score, int depth) {
    return ((score + INF) << 8) | (depth & 0xFF);
}

static int tt_decode_score(int packed) {
    return (packed >> 8) - INF;
}

static int tt_decode_depth(int packed) {
    return packed & 0xFF;
}

static AvlTree g_tt;

static void tt_clear(void) {
    avl_destroy(&g_tt);
    avl_init(&g_tt);
}

static bool tt_lookup(uint64_t hash, int depth_needed, int *score_out) {
    int packed;
    if (!avl_find(&g_tt, hash, &packed))
        return false;
    if (tt_decode_depth(packed) < depth_needed)
        return false;
    *score_out = tt_decode_score(packed);
    return true;
}

static void tt_store(uint64_t hash, int depth, int score) {
    int existing;
    if (avl_find(&g_tt, hash, &existing)) {
        if (tt_decode_depth(existing) > depth)
            return;
    }
    avl_insert(&g_tt, hash, tt_encode(score, depth));
}

// Fonction d'évaluation
static int evaluate(const GameState *state, Player me)
{
    if (game_is_terminal(state)) {
        int s_me = game_score(state, me);
        int s_opp = game_score(state, 1 - me);
        if (s_me > s_opp) return  INF;
        if (s_me < s_opp) return -INF;
        return 0;
    }

    int piece_diff = game_score(state, me) - game_score(state, 1 - me);

    Move tmp[MAX_MOVES];
    int  my_moves, opp_moves;

    if (state->current_player == me) {
        my_moves  = game_generate_moves(state, tmp, MAX_MOVES);
        GameState copy = *state;
        copy.current_player = 1 - me;
        opp_moves = game_generate_moves(&copy, tmp, MAX_MOVES);
    } else {
        opp_moves = game_generate_moves(state, tmp, MAX_MOVES);
        GameState copy = *state;
        copy.current_player = me;
        my_moves  = game_generate_moves(&copy, tmp, MAX_MOVES);
    }

    int mobility_diff = my_moves - opp_moves;

    return 10 * piece_diff + 1 * mobility_diff;
}

// Alpha Beta
static int alpha_beta(const GameState *state,
                      int depth,
                      int alpha,
                      int beta,
                      Player me)
{
    uint64_t hash = game_hash(state);

    int cached_score;
    if (tt_lookup(hash, depth, &cached_score))
        return cached_score;

    if (depth == 0 || game_is_terminal(state)) {
        int score = (state->current_player == me)
                    ?  evaluate(state, me)
                    : -evaluate(state, me);
        tt_store(hash, depth, score);
        return score;
    }

    Move moves[MAX_MOVES];
    int  count = game_generate_moves(state, moves, MAX_MOVES);

    if (count == 0) {
        Move pass = {0, 0, 0, 0, true};
        GameState child = *state;
        game_apply_move(&child, pass);
        int score = -alpha_beta(&child, depth - 1, -beta, -alpha, me);
        tt_store(hash, depth, score);
        return score;
    }

    int best = -INF;

    for (int i = 0; i < count; i++) {
        GameState child = *state;
        game_apply_move(&child, moves[i]);

        int score = -alpha_beta(&child, depth - 1, -beta, -alpha, me);

        if (score > best)  best  = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    tt_store(hash, depth, best);
    return best;
}

// Point d'entrée
Move agent_choose_move(const GameState *state, AgentContext *context)
{
    int depth = (context && context->depth_limit > 0)
                ? context->depth_limit
                : 4;

    tt_clear();

    Move moves[MAX_MOVES];
    int  count = game_generate_moves(state, moves, MAX_MOVES);

    if (count <= 0) {
        Move pass = {0, 0, 0, 0, true};
        return pass;
    }

    Player me = state->current_player;
    Move best_move = moves[0];
    int best_score = -INF;

    for (int i = 0; i < count; i++) {
        GameState child = *state;
        game_apply_move(&child, moves[i]);

        int score = -alpha_beta(&child, depth - 1, -INF, INF, me);

        if (score > best_score) {
            best_score = score;
            best_move = moves[i];
        }
    }

    return best_move;
}

// Compatibilité
Move agent_random_choose_move(const GameState *state)
{
    AgentContext ctx = {.depth_limit = 4};
    return agent_choose_move(state, &ctx);
}
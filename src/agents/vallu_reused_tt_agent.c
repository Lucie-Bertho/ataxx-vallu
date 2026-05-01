#include "agent.h"
#include "game.h"
#include "avl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// Auteurs : Lucie Bettho et Valérian Conquer
// Constantes
#define MAX_MOVES ATAXX_MAX_MOVES
#define INF 1000000
#define HISTORY_SIZE 200

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

// Constante de taille maximale
#define TT_MAX_NODES (1 << 20)   // 1 048 576 nœuds × 32 octets = 32 Mo

static AvlTree g_tt;
static int     g_tt_size = 0;   // nombre de nœuds actuellement stockés

// Suppression de tt_clear() — la table est maintenant persistante.
// On l'initialise une seule fois au premier appel.
static bool g_tt_initialized = false;

static void tt_init_once(void) {
    if (!g_tt_initialized) {
        avl_init(&g_tt);
        g_tt_initialized = true;
    }
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
        // Nœud existant : mise à jour en place si la profondeur est meilleure.
        // Pas de nouveau nœud alloué, g_tt_size reste inchangé.
        if (tt_decode_depth(existing) >= depth)
            return;
        avl_insert(&g_tt, hash, tt_encode(score, depth)); // écrase
        return;
    }

    // Nouveau nœud : on vérifie le budget mémoire
    if (g_tt_size >= TT_MAX_NODES)
        return;  // table pleine, on ignore

    avl_insert(&g_tt, hash, tt_encode(score, depth));
    g_tt_size++;
}
// Compte le nombre de cases vides adjacentes à mes pions
static int count_reachable_empty(const GameState *state, Player me)
{
    int count = 0;
    int size  = state->board_size;

    for (int r = 0; r < size; r++) {
        for (int c = 0; c < size; c++) {
            if (state->board[r][c] != PLAYER_NONE) continue;

            /* cette case vide est-elle adjacente (distance 1) à un de mes pions ? */
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if (nr < 0 || nr >= size || nc < 0 || nc >= size) continue;
                    if (state->board[nr][nc] == me) { count++; goto next_cell; }
                }
            }
            next_cell:;
        }
    }
    return count;
}
// Compte les pions adverses adjacents à (row, col).
// Ce sont exactement les pions qui seront convertis par un coup en (row, col).
static int count_captures(const GameState *state, int row, int col, Player me)
{
    int captures = 0;
    int size = state->board_size;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = row + dr, nc = col + dc;
            if (nr < 0 || nr >= size || nc < 0 || nc >= size) continue;
            if (state->board[nr][nc] == (1 - me)) captures++;
        }
    }
    return captures;
}

// Heuristique d'un coup pour le tri calculé en temps constant (plus grand = exploré en premier).
static int move_score(const GameState *state, const Move *m, Player me)
{
    if (m->is_pass) return -1;

    int captures = count_captures(state, m->to_row, m->to_col, me);

    int dr = abs(m->to_row - m->from_row);
    int dc = abs(m->to_col - m->from_col);
    int is_clone = (dr <= 1 && dc <= 1) ? 1 : 0;

    return captures * 10 + is_clone;
}

// Tri par insertion qui est assez efficace pour b ≈ 40 coups
static void sort_moves(const GameState *state,
                       Move *moves, int count, Player me)
{
    for (int i = 1; i < count; i++) {
        Move key   = moves[i];
        int  s_key = move_score(state, &key, me);
        int  j     = i - 1;
        while (j >= 0 && move_score(state, &moves[j], me) < s_key) {
            moves[j + 1] = moves[j];
            j--;
        }
        moves[j + 1] = key;
    }
}
// Fonction d'évaluation
/*
 * Critère 1 : différence de pions (poids 10)
 *   Dans Ataxx, le gagnant est celui qui a le plus de pions en fin de
 *   partie. Maximiser la différence de pions est donc l'objectif direct.
 *   Poids 10 pour que ce critère domine les autres.
 *
 * Critère 2 : différence de mobilité (poids 1)
 *   Un joueur avec plus de coups disponibles contrôle mieux le plateau
 *   et peut forcer l'adversaire à se déplacer loin (coup saut = pas de
 *   conversion). Poids faible (1) car la mobilité est volatile : elle
 *   change beaucoup d'un tour à l'autre et ne prédit pas fiablement
 *   le résultat final.
 *
 * Critère 3 : cases vides adjacentes à mes pions (poids 3)
 *   On compte les cases vides que je peux atteindre en un coup clone
 *   (distance 1). Plus j'en ai, plus je contrôle l'expansion future.
 *   Cela pousse l'agent à remplir les cases vides tôt plutôt que
 *   de se retrouver bloqué avec des îlots inaccessibles.
 * 
 * Position terminale : ±INF pour guider la recherche vers les victoires
 * et loin des défaites, quelle que soit la profondeur restante.
 */
static int evaluate(const GameState *state, Player me)
{
    // Terminaison
    if (game_is_terminal(state)) {
        int s_me = game_score(state, me);
        int s_opp = game_score(state, 1 - me);
        if (s_me > s_opp) return  INF;
        if (s_me < s_opp) return -INF;
        return 0;
    }

    // Différence de pions
    int piece_diff = game_score(state, me) - game_score(state, 1 - me);

    // Différence de mobilité
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

    // Nombre de cases vides accessibles
    int reachable_diff = count_reachable_empty(state, me)
                   - count_reachable_empty(state, 1 - me);

    // poids 10 pour la différence de pions, 1 pour la mobilité, 3 pour les cases accessibles
    return 10 * piece_diff + 1 * mobility_diff + 3 * reachable_diff;
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

    sort_moves(state, moves, count, me);

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

// Historique des positions de la partie
static uint64_t g_history[HISTORY_SIZE];

static void history_record(const GameState *state) {
    int t = state->turn_count;
    if (t >= 0 && t < HISTORY_SIZE)
        g_history[t] = game_hash(state);
}

static bool history_seen(uint64_t hash, int current_turn) {
    for (int i = 0; i < current_turn && i < HISTORY_SIZE; i++)
        if (g_history[i] == hash) return true;
    return false;
}

// Point d'entrée
Move agent_choose_move(const GameState *state, AgentContext *context)
{
    int depth = (context && context->depth_limit > 0)
                ? context->depth_limit : 4;

    history_record(state);

    tt_init_once();

    Move moves[MAX_MOVES];
    int  count = game_generate_moves(state, moves, MAX_MOVES);

    if (count <= 0) {
        Move pass = {0, 0, 0, 0, true};
        return pass;
    }

    Player me = state->current_player;
    Move best_move = moves[0];
    int best_score = -INF;

    // Détecte si on est dans un cycle depuis N tours
    bool in_cycle = false;
    if (state->turn_count >= 4) {
        uint64_t cur = game_hash(state);
        int t = state->turn_count;
        if (t - 2 >= 0 && t - 2 < HISTORY_SIZE && g_history[t - 2] == cur)
            in_cycle = true;
        if (t - 4 >= 0 && t - 4 < HISTORY_SIZE && g_history[t - 4] == cur)
            in_cycle = true;
    }

    for (int i = 0; i < count; i++) {
        GameState child = *state;
        game_apply_move(&child, moves[i]);

        int score = -alpha_beta(&child, depth - 1, -INF, INF, me);

        // Pénalité pour les positions déjà vues dans l'historique récent
        if (history_seen(game_hash(&child), state->turn_count))
            score -= 5000;

        // Bonus pour les coups sauts (pour casser les cycles)
        if (in_cycle && moves[i].from_row != moves[i].to_row
                     && moves[i].from_col != moves[i].to_col) {
            score += 300;
        }

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
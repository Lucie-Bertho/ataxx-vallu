#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct AvlNode {
    uint64_t key;
    struct AvlNode* left;
    struct AvlNode* right;
    int height;
    int value;
} AvlNode;

int main() {
    AvlNode node;
    node.key = 42;
    node.left = 0;
    node.right = 0;
    node.height = 1;
    node.value = 100;
    
    printf("Node key: %lu, size: %d\n", node.key, sizeof(node));
    
    return 0;
}
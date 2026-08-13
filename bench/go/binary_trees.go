package main

import (
	"fmt"
	"time"
)

func buildTree(tree []int, d, pos, val int) {
	tree[pos] = val
	if d > 0 {
		buildTree(tree, d-1, 2*pos+1, val-1)
		buildTree(tree, d-1, 2*pos+2, val-1)
	}
}

func itemCheck(tree []int, pos, size int) int {
	if 2*pos+2 >= size {
		return tree[pos]
	}
	return tree[pos] + itemCheck(tree, 2*pos+1, size) + itemCheck(tree, 2*pos+2, size)
}

func binTrees(maxDepth int) int64 {
	size := (1 << (maxDepth + 1)) - 1
	tree := make([]int, size)
	var check int64
	for d := 4; d <= maxDepth; d += 2 {
		buildTree(tree, d, 0, d)
		check += int64(itemCheck(tree, 0, size))
	}
	return check
}

func main() {
	t0 := time.Now()
	v := binTrees(17)
	fmt.Printf("verify %d\nms %d\n", v, int(time.Since(t0).Milliseconds()))
}

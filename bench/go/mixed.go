// bench/go/mixed.go — 对象/容器/string/数组 混合负载（与 myp/mixed.myp 对拍）
// Go 侧等价：短命对象 churn + slice 容器 + strconv 拼接 + 可变长 make([]int)。
// verify 与 MYP 完全一致；输出 verify/ms 供 run_compare_go.sh 解析。
package main

import (
	"fmt"
	"strconv"
	"time"
)

type Node struct {
	v   int
	tag string
}

func run(n int64) int64 {
	var chk int64
	for i := int64(0); i < n; i++ {
		// ① 短命对象 churn
		o := &Node{v: 1, tag: "t"}
		chk += int64(o.v)
		// ② 容器：8 元素追加 + 置 nil（交给 GC）
		lst := make([]*Node, 0, 8)
		for j := 0; j < 8; j++ {
			lst = append(lst, &Node{v: int(i) + j, tag: "tag"})
		}
		chk += int64(len(lst))
		lst = nil
		// ③ string 拼接（strconv + concat）
		t := "i=" + strconv.FormatInt(i, 10) + ".x" + strconv.FormatInt(i%10, 10)
		chk += int64(len(t))
		// ④ 可变长动态数组
		a := make([]int, int(i%8)+1)
		a[0] = int(i)
		chk += int64(a[0])
	}
	return chk
}

func main() {
	const n int64 = 200000
	t0 := time.Now()
	v := run(n)
	fmt.Printf("verify %d\nms %d\n", v, int(time.Since(t0).Milliseconds()))
}

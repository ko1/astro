// celgo_ref — minimal cel-go-backed reference evaluator used by arcel's
// test harness and benchmarks.
//
// Usage forms:
//
//   celgo_ref eval -e '<expr>' [-i '<json>']
//       Evaluate <expr> with optional JSON input bound to top-level
//       variables (object keys become CEL identifiers).  Prints the
//       result as JSON, or "ERROR: <msg>" to stdout on eval / compile
//       failure.  Exit 0 always so we can compare against arcel.
//
//   celgo_ref bench -e '<expr>' [-i '<json>'] -n <iterations>
//       Compile once, evaluate <iterations> times, print elapsed
//       wall time in nanoseconds (just the inner loop, not parse/build).
//
//   celgo_ref repl
//       Read 'expr<TAB>json' lines on stdin, print one result per line.
//       Used by the test harness to amortize Go startup over many cases.
//
// JSON values are mapped to CEL types using cel-go's defaults (numbers
// become double, etc.), then bindings can pin them via `--int <name>`
// flags if needed (not implemented yet — fixtures we care about are
// type-flexible).
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/google/cel-go/cel"
	"github.com/google/cel-go/common/types/ref"
)

func mustEnv(vars map[string]any) (*cel.Env, []cel.EnvOption) {
	opts := []cel.EnvOption{}
	for name := range vars {
		opts = append(opts, cel.Variable(name, cel.DynType))
	}
	env, err := cel.NewEnv(opts...)
	if err != nil {
		fmt.Fprintf(os.Stderr, "env error: %v\n", err)
		os.Exit(2)
	}
	return env, opts
}

func compile(env *cel.Env, expr string) (cel.Program, error) {
	ast, iss := env.Compile(expr)
	if iss.Err() != nil {
		return nil, iss.Err()
	}
	prg, err := env.Program(ast)
	if err != nil {
		return nil, err
	}
	return prg, nil
}

func bindingsFromJSON(input string) (map[string]any, error) {
	if input == "" {
		return map[string]any{}, nil
	}
	var v any
	if err := json.Unmarshal([]byte(input), &v); err != nil {
		return nil, fmt.Errorf("input JSON parse: %v", err)
	}
	if obj, ok := v.(map[string]any); ok {
		return obj, nil
	}
	return map[string]any{"input": v}, nil
}

func formatResult(r ref.Val) string {
	v := r.Value()
	switch tv := v.(type) {
	case string:
		b, _ := json.Marshal(tv)
		return string(b)
	case bool:
		if tv {
			return "true"
		}
		return "false"
	case int64:
		return fmt.Sprintf("%d", tv)
	case uint64:
		return fmt.Sprintf("%du", tv)
	case float64:
		b, _ := json.Marshal(tv)
		return string(b)
	case nil:
		return "null"
	default:
		b, err := json.Marshal(tv)
		if err != nil {
			return fmt.Sprintf("%v", tv)
		}
		return string(b)
	}
}

func cmdEval(args []string) {
	fs := flag.NewFlagSet("eval", flag.ExitOnError)
	expr := fs.String("e", "", "CEL expression (required)")
	input := fs.String("i", "", "JSON input (object keys → CEL variables)")
	fs.Parse(args)
	if *expr == "" {
		fmt.Fprintln(os.Stderr, "eval: -e <expr> is required")
		os.Exit(2)
	}
	binds, err := bindingsFromJSON(*input)
	if err != nil {
		fmt.Println("ERROR: " + err.Error())
		return
	}
	env, _ := mustEnv(binds)
	prg, err := compile(env, *expr)
	if err != nil {
		fmt.Println("ERROR: " + err.Error())
		return
	}
	out, _, err := prg.Eval(binds)
	if err != nil {
		fmt.Println("ERROR: " + err.Error())
		return
	}
	fmt.Println(formatResult(out))
}

func cmdBench(args []string) {
	fs := flag.NewFlagSet("bench", flag.ExitOnError)
	expr := fs.String("e", "", "CEL expression (required)")
	input := fs.String("i", "", "JSON input")
	n := fs.Int("n", 1000000, "iteration count")
	fs.Parse(args)
	if *expr == "" {
		fmt.Fprintln(os.Stderr, "bench: -e <expr> is required")
		os.Exit(2)
	}
	binds, err := bindingsFromJSON(*input)
	if err != nil {
		fmt.Fprintln(os.Stderr, "input parse: ", err)
		os.Exit(2)
	}
	env, _ := mustEnv(binds)
	prg, err := compile(env, *expr)
	if err != nil {
		fmt.Fprintln(os.Stderr, "compile: ", err)
		os.Exit(2)
	}
	// warm
	for i := 0; i < 1000; i++ {
		_, _, _ = prg.Eval(binds)
	}
	start := time.Now()
	for i := 0; i < *n; i++ {
		_, _, _ = prg.Eval(binds)
	}
	elapsed := time.Since(start)
	nsPerOp := float64(elapsed.Nanoseconds()) / float64(*n)
	fmt.Printf("%d %d %.3f\n", *n, elapsed.Nanoseconds(), nsPerOp)
}

// repl: read one JSON envelope per line on stdin, print one result
// line on stdout.  Input shape:
//
//	{"e": "<cel expr>", "i": <json input value or null>}
//
// We use a JSON envelope rather than raw TAB-separated form because
// CEL string literals legitimately contain '\n' / '\t' (the textproto
// suite has many) and naive line splitting desynchronizes the stream.
//
// Output is a single line — a JSON literal on success, or `ERROR: <msg>`
// (with embedded newlines flattened) on failure.  Exactly one output
// line per input line.
type replIn struct {
	E string          `json:"e"`
	I json.RawMessage `json:"i"`
}

func cmdRepl(_ []string) {
	rd := bufio.NewReaderSize(os.Stdin, 1<<20)
	w := bufio.NewWriterSize(os.Stdout, 1<<20)
	defer w.Flush()
	dec := json.NewDecoder(rd)
	for {
		var in replIn
		if err := dec.Decode(&in); err != nil {
			return // EOF or malformed → exit cleanly
		}
		runOne(in, w)
		_ = w.Flush()
	}
}

func runOne(in replIn, w *bufio.Writer) {
	input := ""
	if len(in.I) > 0 && string(in.I) != "null" {
		input = string(in.I)
	}
	binds, err := bindingsFromJSON(input)
	if err != nil {
		fmt.Fprintln(w, "ERROR: "+singleLine(err.Error()))
		return
	}
	env, _ := mustEnv(binds)
	prg, err := compile(env, in.E)
	if err != nil {
		fmt.Fprintln(w, "ERROR: "+singleLine(err.Error()))
		return
	}
	out, _, err := prg.Eval(binds)
	if err != nil {
		fmt.Fprintln(w, "ERROR: "+singleLine(err.Error()))
		return
	}
	fmt.Fprintln(w, formatResult(out))
}

// singleLine flattens cel-go's multi-line error messages (which carry
// source-snippet pointers) into a single line so the test harness's
// line-oriented protocol stays synchronized.
func singleLine(s string) string {
	return strings.ReplaceAll(strings.ReplaceAll(s, "\r", " "), "\n", " | ")
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: celgo_ref {eval|bench|repl} [args...]")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "eval":
		cmdEval(os.Args[2:])
	case "bench":
		cmdBench(os.Args[2:])
	case "repl":
		cmdRepl(os.Args[2:])
	default:
		fmt.Fprintf(os.Stderr, "unknown subcommand: %s\n", os.Args[1])
		os.Exit(2)
	}
}

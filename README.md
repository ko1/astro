# ASTro: AST-based Reusable Optimizer

Note: This project is still experimental, and the API is subject to significant changes.

ASTro is an optimization framework based on **Abstract Syntax Trees (ASTs)**.
It provides a reusable infrastructure for generating optimized code fragments through partial evaluation of AST interpreters.

The companion tool **ASTroGen** automatically generates an interpreter (evaluator) and specializers (partial evaluators) from a `node.def` file, which defines node types and their behaviors in C code.

See the [`./sample`](./sample) directory for examples:

- [`./sample/calc/`](./sample/calc/): A toy "Calc" language with three types of AST nodes.  
- [`./sample/naruby/`](./sample/naruby/): "Not a Ruby": a minimal Ruby subset implemented for demonstration.

## Reference

[ASTro: An AST-Based Reusable Optimization Framework | Proceedings of the 17th ACM SIGPLAN International Workshop on Virtual Machines and Intermediate Languages](https://dl.acm.org/doi/10.1145/3759548.3763371)


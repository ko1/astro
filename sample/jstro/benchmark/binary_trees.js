// binary-trees from CLBG.  Allocation-heavy.
function TreeNode(left, right) {
  this.left = left;
  this.right = right;
}
function itemCheck(tree) {
  if (tree.left === null) return 1;
  return 1 + itemCheck(tree.left) + itemCheck(tree.right);
}
function bottomUpTree(depth) {
  if (depth > 0) {
    return new TreeNode(bottomUpTree(depth - 1), bottomUpTree(depth - 1));
  }
  return new TreeNode(null, null);
}

var n = 14;     // tune for ~1s
var minDepth = 4;
var maxDepth = Math.max(minDepth + 2, n);
var stretchDepth = maxDepth + 1;
var start = Date.now();

var stretch = bottomUpTree(stretchDepth);
console.log("stretch depth " + stretchDepth + ": check " + itemCheck(stretch));

var longLived = bottomUpTree(maxDepth);
for (var depth = minDepth; depth <= maxDepth; depth += 2) {
  var iterations = 1 << (maxDepth - depth + minDepth);
  var check = 0;
  for (var i = 1; i <= iterations; i++) {
    check += itemCheck(bottomUpTree(depth));
  }
  console.log(iterations + " trees of depth " + depth + ": check " + check);
}
console.log("long-lived check " + itemCheck(longLived));
var elapsed = (Date.now() - start) / 1000;
console.log("elapsed: " + elapsed + "s");

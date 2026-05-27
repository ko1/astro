# Binary trees (recursive allocation + recursive walk)
class TreeNode
  def initialize(left, right)
    @left = left
    @right = right
  end
  def check
    if @left == nil
      1
    else
      1 + @left.check + @right.check
    end
  end
end

def make_tree(depth)
  if depth == 0
    TreeNode.new(nil, nil)
  else
    TreeNode.new(make_tree(depth - 1), make_tree(depth - 1))
  end
end

max_depth = 20
tree = make_tree(max_depth)
p(tree.check)

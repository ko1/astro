# Binary search tree built from classes; in-order traversal sums values.
class TreeNode(object):
    value:int = 0
    left:"TreeNode" = None
    right:"TreeNode" = None
    def insert(self:"TreeNode", v:int) -> object:
        if v < self.value:
            if self.left is None:
                self.left = makeNode(v)
            else:
                self.left.insert(v)
        else:
            if self.right is None:
                self.right = makeNode(v)
            else:
                self.right.insert(v)
        return None
    def sum(self:"TreeNode") -> int:
        s:int = 0
        s = self.value
        if not (self.left is None):
            s = s + self.left.sum()
        if not (self.right is None):
            s = s + self.right.sum()
        return s
    def height(self:"TreeNode") -> int:
        lh:int = 0
        rh:int = 0
        if not (self.left is None):
            lh = self.left.height()
        if not (self.right is None):
            rh = self.right.height()
        if lh > rh:
            return lh + 1
        return rh + 1

def makeNode(v:int) -> TreeNode:
    n:TreeNode = None
    n = TreeNode()
    n.value = v
    return n

root:TreeNode = None
data:[int] = None
i:int = 0

root = makeNode(50)
data = [30, 70, 20, 40, 60, 80, 10]
i = 0
while i < len(data):
    root.insert(data[i])
    i = i + 1
print(root.sum())
print(root.height())

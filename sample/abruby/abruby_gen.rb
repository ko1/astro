require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"
  end
end

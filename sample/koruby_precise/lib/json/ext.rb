# frozen_string_literal: true

require 'json/common'

module JSON
  # koruby port: there are no C extensions; json/ext/parser.rb and
  # json/ext/generator.rb are pure-Ruby ports of parser.c and generator.c.
  module Ext
    class Parser
      class << self
        def parse(...)
          new(...).parse
        end
        alias_method :parse, :parse # Allow redefinition by extensions
      end

      def initialize(source, opts = nil)
        @source = source
        @config = Config.new(opts)
      end

      def source
        @source.dup
      end

      def parse
        @config.parse(@source)
      end
    end

    require 'json/ext/parser'
    Ext::Parser::Config = Ext::ParserConfig
    JSON.parser = Ext::Parser

    # koruby port: json/ext/generator.rb is a pure-Ruby port of generator.c.
    require 'json/ext/generator'
    JSON.generator = Generator
  end

  if defined?(ResumableParser) # Not yet available on JRuby
    class ResumableParser
      # Returns whether the parser is entirely done: no unconsumed bytes in
      # the buffer, no document under construction and no parsed value
      # awaiting retrieval.
      #
      # The main use case is detecting a truncated stream once the input is
      # exhausted:
      #
      #   loop do
      #     begin
      #       parser << socket.readpartial(4096)
      #     rescue EOFError
      #       break
      #     end
      #     while parser.parse
      #       process(parser.value)
      #     end
      #   end
      #   warn "stream was truncated" unless parser.empty?
      def empty?
        eos? && !partial_value? && !value?
      end
    end
  end
end

# typed: true
# enable-experimental-requires-ancestor: true

module Helper
  extend T::Helpers

  requires_ancestor { T.class_of(Foo) }

  def helper
    foo
  end
end

class Foo
  class << self
    include Helper

    def foo
    end

    def bar
      helper
    end
  end
end

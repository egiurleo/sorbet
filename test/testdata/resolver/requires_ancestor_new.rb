# typed: true
# enable-experimental-requires-ancestor: true

class Foo; end

module Helper
  extend T::Helpers

  requires_ancestor { T.class_of(Foo) }
end

// Copyright Louis Dionne 2016
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#ifndef TE_HPP
#define TE_HPP

#include <boost/hana/at_key.hpp>
#include <boost/hana/core/tag_of.hpp>
#include <boost/hana/map.hpp>
#include <boost/hana/pair.hpp>
#include <boost/hana/string.hpp>
#include <boost/hana/type.hpp>

#include <boost/callable_traits/function_type.hpp>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>


namespace te {
// Encapsulates the minimal amount of information required to allocate
// storage for an object of a given type.
//
// This should never be created explicitly; always use `te::type_info_for`.
struct type_info {
  std::size_t size;
  std::size_t alignment;
};

template <typename T>
constexpr type_info type_info_for{sizeof(T), alignof(T)};

// Class implementing the small buffer optimization (SBO).
//
// This class represents a value of an unknown type that is stored either on
// the heap, or on the stack if it fits in the specific small buffer size.
//
// This class is a building block for other, more complex utilities. It handles
// only the logic related to the storage of the object, but __nothing__ else.
// In particular, the object stored in this class is never constructed and
// never destroyed (but we do reclaim heap storage if it was allocated).
//
// Indeed, properly handling construction and destruction would require the
// `small_buffer` to remember the constructor/destructor that must be called,
// which is beyond its scope. Instead, the user of `small_buffer` is expected
// to set up some virtual dispatching mechanism on its own, which should
// include virtual construction/destruction if desirable.
//
// General usage goes like this:
//
//    small_buffer<8> buf{te::type_info_for<std::string>};
//    // `buf` is able to hold a string, maybe in the small buffer or maybe on
//    // the heap. However, `buf` does not contain anything at this point and
//    // it does not remember the type that it can contain. Constructing and
//    // destroying are the responsibility of the user.
//    new (buf.get()) std::string{"abcdef"};  // may be on the heap or on the stack
//    std::string* p = buf.get<std::string>();
//    buf.get<std::string>()->~std::string(); // must be called manually too
//
// The nice thing about `small_buffer` is that it has a single type and its
// ABI does not change even if the type of what it holds changes. However, it
// provides this at the cost of "forgetting" this information, and type erasure
// techniques must be used on top of `small_buffer` to do anything useful.
template <std::size_t Size, std::size_t Align = static_cast<std::size_t>(-1)>
class small_buffer {
  static constexpr std::size_t SBSize = Size < sizeof(void*) ? sizeof(void*) : Size;
  static constexpr std::size_t SBAlign = Align == -1 ? alignof(std::aligned_storage_t<SBSize>) : Align;
  using SBStorage = std::aligned_storage_t<SBSize, SBAlign>;

  static constexpr bool can_use_sbo(std::size_t size, std::size_t alignment) {
    return size <= sizeof(SBStorage) && alignof(SBStorage) % alignment == 0;
  }

  union {
    void* ptr_;
    SBStorage sb_;
  };
  // TODO: It might be possible to pack this bool inside the union somehow.
  bool uses_heap_;

public:
  small_buffer() = delete;
  small_buffer& operator=(small_buffer const&) = delete;

  explicit small_buffer(te::type_info info) {
    // TODO: We could also construct the object at an aligned address within
    // the buffer, which would require computing the right address everytime
    // we access the buffer as a T, but would allow more Ts to fit in the SBO.
    if (can_use_sbo(info.size, info.alignment)) {
      uses_heap_ = false;
    } else {
      uses_heap_ = true;
      ptr_ = std::malloc(info.size);
      // TODO: That's not a really nice way to handle this
      assert(ptr_ != nullptr && "std::malloc failed, we're doomed");
    }
  }

  ~small_buffer() {
    if (uses_heap())
      std::free(ptr_);
  }

  bool uses_heap() const { return uses_heap_; }

  template <typename T = void>
  T* get() {
    return static_cast<T*>(uses_heap() ? ptr_ : &sb_);
  }

  template <typename T = void>
  T const* get() const {
    return static_cast<T const*>(uses_heap() ? ptr_ : &sb_);
  }
};

// Placeholder type representing the type of ref-unqualified `*this`
// when defining vtables.
struct T;

namespace detail {
  // Transform a signature from the way it is specified in a concept definition
  // to a type suitable for storage in a vtable.
  template <typename T>
  struct sig_replace {
    static_assert(!std::is_same<T, te::T>{},
      "te::T may not be passed by value in concept definitions; it is only a placeholder");
    using type = T;
  };
  template <typename R, typename ...Args>
  struct sig_replace<R (Args...)> {
    using type = typename sig_replace<R>::type (*)(typename sig_replace<Args>::type...);
  };
  template <>
  struct sig_replace<te::T const&> {
    using type = void const*;
  };
  template <>
  struct sig_replace<te::T &> {
    using type = void*;
  };
  template <>
  struct sig_replace<te::T &&> {
    using type = void*;
  };
  template <>
  struct sig_replace<te::T *> {
    using type = void*;
  };
  template <>
  struct sig_replace<te::T const *> {
    using type = void const*;
  };

  // Cast an argument from a generic representation to the actual type expected
  // by a statically typed equivalent.
  //
  // In what's below, `Ac` stands for `Actual`, i.e. the actual static type
  // being requested by the function as defined in the concept map.
  template <typename Ac>
  constexpr Ac const& special_cast_impl(boost::hana::basic_type<te::T const&>,
                                        boost::hana::basic_type<Ac const&>,
                                        void const* arg)
  { return *static_cast<Ac const*>(arg); }
  template <typename Ac>
  constexpr Ac& special_cast_impl(boost::hana::basic_type<te::T &>,
                                  boost::hana::basic_type<Ac &>,
                                  void* arg)
  { return *static_cast<Ac*>(arg); }
  template <typename Ac>
  constexpr Ac&& special_cast_impl(boost::hana::basic_type<te::T &&>,
                                   boost::hana::basic_type<Ac &&>,
                                   void* arg)
  { return std::move(*static_cast<Ac*>(arg)); }
  template <typename Ac>
  constexpr Ac* special_cast_impl(boost::hana::basic_type<te::T *>,
                                  boost::hana::basic_type<Ac *>,
                                  void* arg)
  { return static_cast<Ac*>(arg); }
  template <typename Ac>
  constexpr Ac const* special_cast_impl(boost::hana::basic_type<te::T const*>,
                                        boost::hana::basic_type<Ac const*>,
                                        void const* arg)
  { return static_cast<Ac const*>(arg); }
  template <typename Req, typename Ac, typename Arg>
  constexpr Req special_cast_impl(boost::hana::basic_type<Req>,
                                  boost::hana::basic_type<Ac>,
                                  Arg&& arg)
  { return std::forward<Arg>(arg); }
  template <typename Pl, typename Ac, typename Arg>
  constexpr decltype(auto) special_cast(Arg&& arg) {
    return detail::special_cast_impl(boost::hana::basic_type<Pl>{},
                                     boost::hana::basic_type<Ac>{},
                                     std::forward<Arg>(arg));
  }

  // Transform an actual (stateless) function object with statically typed
  // parameters into a type-erased function object suitable for storage in
  // a vtable.
  template <typename R_pl, typename ...Args_pl,
            typename R_ac, typename ...Args_ac,
            typename Function>
  constexpr auto fun_replace(boost::hana::basic_type<R_pl(Args_pl...)> /*placeholder_sig*/,
                             boost::hana::basic_type<R_ac(Args_ac...)> /*actual_sig*/,
                             Function)
  {
    using Storage = typename sig_replace<R_pl(Args_pl...)>::type;
    auto adapter = [](typename sig_replace<Args_pl>::type ...args)
      -> typename sig_replace<R_pl>::type
    {
      static_assert(std::is_empty<Function>{},
        "This trick won't work if `Function` is not empty.");
      return detail::special_cast<R_pl, R_ac>(
        (*static_cast<Function*>(nullptr))( // <-------------- UB ALERT
          detail::special_cast<Args_pl, Args_ac>(args)...
        )
      );
    };
    return static_cast<Storage>(adapter);
  }
  template <typename ...Args_pl, typename ...Args_ac, typename Function>
  constexpr auto fun_replace(boost::hana::basic_type<void(Args_pl...)> /*placeholder_sig*/,
                             boost::hana::basic_type<void(Args_ac...)> /*actual_sig*/,
                             Function)
  {
    using Storage = typename sig_replace<void(Args_pl...)>::type;
    auto adapter = [](typename sig_replace<Args_pl>::type ...args) -> void {
      static_assert(std::is_empty<Function>{},
        "This trick won't work if `Function` is not empty.");
      (*static_cast<Function*>(nullptr))( // <-------------- UB ALERT
        detail::special_cast<Args_pl, Args_ac>(args)...
      );
    };
    return static_cast<Storage>(adapter);
  }
} // end namespace detail

// Class implementing a simple vtable.
template <typename ...Functions>
struct vtable;

template <typename ...Name, typename ...Signature>
struct vtable<boost::hana::pair<Name, boost::hana::basic_type<Signature>>...> {
  template <typename VTable>
  constexpr explicit vtable(VTable vtable)
    : vtbl_{boost::hana::make_map(
      boost::hana::make_pair(
        Name{},
        detail::fun_replace(
          boost::hana::basic_type<Signature>{},
          boost::hana::basic_type<
            boost::callable_traits::function_type_t<
              std::decay_t<decltype(vtable[Name{}])>
            >
          >{},
          vtable[Name{}]
        )
      )...
    )}
  { }

  template <typename Name_>
  constexpr auto operator[](Name_ name) const {
    return vtbl_[name];
  }

private:
  boost::hana::map<
    boost::hana::pair<Name, typename detail::sig_replace<Signature>::type>...
  > vtbl_;
};

// Never defined; just a helper to use as decltype(make_vtable(...)) when
// defining concepts.
template <typename ...Name, typename ...Signature>
vtable<boost::hana::pair<Name, boost::hana::basic_type<Signature>>...>
make_vtable(boost::hana::pair<Name, boost::hana::basic_type<Signature>>...);

// This one is actually defined, and it should be used to define actual
// concept maps.
template <typename ...Functions>
constexpr auto make_vtable(Functions ...f) {
  return boost::hana::make_map(f...);
}

// Helpers to create a Hana-map using a nicer DSEL.
template <typename Signature>
constexpr boost::hana::basic_type<Signature> function{};

namespace detail {
  template <char ...c>
  struct string : boost::hana::string<c...> {
    template <typename Function>
    constexpr boost::hana::pair<string, Function>
    operator=(Function f) const {
      static_assert(std::is_empty<Function>{},
        "Only stateless function objects can be used to define vtables");
      return {{}, f};
    }
    using hana_tag = typename boost::hana::tag_of<boost::hana::string<c...>>::type;
  };
} // end namespace detail

namespace literals {
  template <typename CharT, CharT ...c>
  constexpr auto operator""_s() { return detail::string<c...>{}; }
} // end namespace literals

} // end namespace te

#endif // TE_HPP
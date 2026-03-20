#pragma once

#include "nref.hpp"

#include <cstddef>
#include <ranges>
#include <type_traits>
#include <utility>

namespace nrx::ref::dsl {

template<class>
struct is_nref : std::false_type {};

template<class R, class O>
struct is_nref<nrx::ref::nref<R, O>> : std::true_type {};

inline constexpr auto deref = std::views::transform([](auto* p) {
    using raw_type = std::remove_pointer_t<decltype(p)>;
    using value_type = std::remove_cvref_t<raw_type>;

    if constexpr (is_nref<value_type>::value) {
        using owned_type = typename value_type::owned_type;
        return static_cast<owned_type>(static_cast<const raw_type&>(*p).get());
    } else {
        return *p;
    }
});

struct lift_stream_t {
    template<class RefT, class OwnedT>
    auto operator()(nrx::ref::nref<RefT, OwnedT>& nr) const {
        return std::ranges::single_view<nrx::ref::nref<RefT, OwnedT>*>{ &nr } | deref;
    }

    template<class RefT, class OwnedT>
    auto operator()(const nrx::ref::nref<RefT, OwnedT>& nr) const {
        return std::ranges::single_view<const nrx::ref::nref<RefT, OwnedT>*>{ &nr } | deref;
    }

    template<class T>
    auto operator()(T* p) const {
        return std::ranges::single_view<T*>{ p };
    }

    template<class T>
    auto operator()(const T* p) const {
        return std::ranges::single_view<const T*>{ p };
    }

    template<class T, std::size_t N>
    auto operator()(T (&arr)[N]) const {
        return std::views::all(arr);
    }

    template<class T, std::size_t N>
    auto operator()(const T (&arr)[N]) const {
        return std::views::all(arr);
    }

    template<class R>
    auto operator()(R&& r) const
        requires std::ranges::viewable_range<std::remove_cvref_t<R>>
    {
        return std::views::all(std::forward<R>(r));
    }

    template<class T>
    auto operator()(T&& v) const
        requires (!std::ranges::input_range<std::remove_cvref_t<T>> &&
                  !std::is_pointer_v<std::remove_reference_t<T>> &&
                  !is_nref<std::remove_cvref_t<T>>::value)
    {
        using value_type = std::remove_cvref_t<T>;
        return std::ranges::single_view<value_type>{ static_cast<value_type>(std::forward<T>(v)) };
    }
};

inline constexpr lift_stream_t lift_stream{};

template<class LHS, class RHS>
requires (!std::ranges::input_range<std::remove_cvref_t<LHS>> &&
          requires (LHS&& l, RHS&& r) {
              lift_stream(std::forward<LHS>(l)) | std::forward<RHS>(r);
          })
auto operator|(LHS&& lhs, RHS&& rhs)
    -> decltype(lift_stream(std::forward<LHS>(lhs)) | std::forward<RHS>(rhs))
{
    return lift_stream(std::forward<LHS>(lhs)) | std::forward<RHS>(rhs);
}

} // namespace nrx::ref::dsl

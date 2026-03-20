/**
 * @file nref.hpp
 * @brief nullable_reference (nref) — 유연한 참조/소유 래퍼
 *
 * nref<T>는 다섯 가지 상태를 동적으로 전환합니다.
 *
 *   bound_live     외부 변수를 참조. 원본 값이 바뀌면 get()도 바뀐 값을 반환.
 *   owned_value    값을 직접 소유.
 *   stored_pointer 포인터 주소 자체를 보관. 값 접근은 get_address()로만 가능.
 *   bound_view     range를 소유하며 매 get() 호출 시 첫 원소를 읽음.
 *   empty          아무것도 없는 상태.
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  상태 전환 방법                                                   │
 * │                                                                   │
 * │  &x                → bound_live                                  │
 * │  값                → owned_value (bound 상태에서는 원본 변수 수정) │
 * │  must_copy(&x)     → stored_pointer                              │
 * │  first_of_range{r} → bound_view                                  │
 * │  std::nullopt      → empty                                       │
 * │  nref*             → 소스 상태에 따라 파생                        │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * bound_live + peer 추적(nref* 대입):
 *   다른 nref의 owned_value를 바인딩한 경우, peer가 소멸한 뒤
 *   get() 또는 복합 대입을 호출하면 std::logic_error를 던집니다.
 *   소멸 전에 값을 보존하려면 detach()를 명시적으로 호출하세요.
 *
 * 스레드 안전성:
 *   - 읽기(get, is_*, current_storage)는 shared_lock으로 병렬 실행 가능합니다.
 *   - 쓰기(operator=, reset, detach, +=, ...)는 unique_lock입니다.
 *   - bound_ptr_가 가리키는 외부 변수의 스레드 안전성은 사용자 책임입니다.
 *
 * 요구 사항: C++20 이상
 */

#pragma once

#include <optional>
#include <functional>
#include <memory>
#include <type_traits>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <mutex>
#include <shared_mutex>

#ifndef NDEBUG
#include <cassert>
#define NRX_ASSERT(x) assert(x)
#else
#define NRX_ASSERT(x) ((void)0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NRX_FORCE_INLINE __attribute__((always_inline)) inline
#else
#define NRX_FORCE_INLINE inline
#endif

namespace nrx::ref {

template<typename RefT>
struct copy_address_request { RefT* ptr_; };

template<typename RefT>
NRX_FORCE_INLINE copy_address_request<RefT> must_copy(RefT* p) { return { p }; }

template<typename RefT, typename OwnedT = RefT>
class nref {
public:
    enum class storage {
        empty,
        bound_live,
        owned_value,
        stored_pointer,
        bound_view,
    };

    using ref_type   = RefT;
    using owned_type = OwnedT;

    static_assert(std::is_convertible_v<RefT,   OwnedT>, "RefT must be convertible to OwnedT");
    static_assert(std::is_convertible_v<OwnedT, RefT>,   "OwnedT must be convertible to RefT");

    template<class R> struct first_of_range { R r_; };
    template<class R> struct snap_range     { R r_; };

    template<typename R2, typename O2> friend class nref;

private:
    // ─────────────────────────────────────────────────────────────────────────
    // 멤버
    //
    // shadow cache 제거:
    //   이전 설계에서 owned_value_는 bound_live 상태에서 shadow cache를 겸했습니다.
    //   peer 소멸 후 마지막 값을 보존하기 위해서였는데, 이제 peer 소멸 후 접근하면
    //   예외를 던지도록 설계를 바꿨습니다. 따라서 owned_value_는 owned_value
    //   상태에서만 사용하며, bound_live에서는 bound_ptr_만 씁니다.
    //   mutable도 불필요해져 제거되었습니다.
    // ─────────────────────────────────────────────────────────────────────────

    RefT*                  bound_ptr_        = nullptr;
    std::optional<OwnedT>  owned_value_      = std::nullopt;  // owned_value 상태 전용
    RefT*                  stored_ptr_value_ = nullptr;
    std::shared_ptr<void>  view_obj_         = {};
    std::function<OwnedT()> view_pull_       = {};

    storage storage_ = storage::empty;

    // peer 수명 추적 (nref* 대입 시에만 활성화)
    std::weak_ptr<void> peer_token_    = {};
    bool                tracking_peer_ = false;

    // 수명 토큰 — lazy init: lifetime_token() 첫 호출 시에만 make_shared
    mutable std::shared_ptr<void> token_ = {};

    mutable std::shared_mutex mtx_;

    // ─────────────────────────────────────────────────────────────────────────
    // Invariant
    // ─────────────────────────────────────────────────────────────────────────
    [[nodiscard]] bool check_invariant_() const noexcept {
        switch (storage_) {
        case storage::empty:          return true;
        case storage::bound_live:     return bound_ptr_ != nullptr;
        case storage::owned_value:    return owned_value_.has_value();
        case storage::stored_pointer: return stored_ptr_value_ != nullptr;
        case storage::bound_view:     return static_cast<bool>(view_pull_);
        }
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 상태 전환 헬퍼 (락 보유 상태에서 호출)
    // ─────────────────────────────────────────────────────────────────────────

    // owned_value 상태를 떠날 때 반드시 호출.
    // token_을 무효화하여 이 객체의 owned_value를 바라보던 peer들이
    // weak_ptr 만료를 즉시 감지하도록 합니다.
    //
    // 예: owner = &x  처럼 owned_value → bound_live 전환 시,
    //     token_을 죽이지 않으면 a.peer_token_이 만료되지 않아
    //     a가 죽은 owned_value_ 주소를 계속 신뢰하는 버그가 생깁니다.
    void revoke_owned_token_if_needed_() noexcept {
        if (storage_ == storage::owned_value) token_.reset();
    }

    void reset_unlocked_() noexcept {
        revoke_owned_token_if_needed_();
        bound_ptr_        = nullptr;
        owned_value_.reset();
        stored_ptr_value_ = nullptr;
        view_obj_.reset();
        view_pull_        = {};
        peer_token_.reset();
        tracking_peer_    = false;
        token_.reset();    // empty 전환이므로 정체성 자체를 소멸
        storage_ = storage::empty;
    }

    // 외부 포인터(&x) 바인딩 — peer 추적 없음
    void assign_ptr_unlocked_(RefT* p) {
        if (!p) { reset_unlocked_(); return; }
        revoke_owned_token_if_needed_();
        bound_ptr_        = p;
        owned_value_.reset();
        stored_ptr_value_ = nullptr;
        view_obj_.reset();
        view_pull_        = {};
        peer_token_.reset();
        tracking_peer_    = false;
        storage_ = storage::bound_live;
    }

    // 다른 nref의 owned_value 내부 주소 바인딩 + peer 수명 추적
    void assign_ptr_with_peer_unlocked_(RefT* p, std::shared_ptr<void> peer_tok) {
        if (!p) { reset_unlocked_(); return; }
        revoke_owned_token_if_needed_();
        bound_ptr_        = p;
        owned_value_.reset();
        stored_ptr_value_ = nullptr;
        view_obj_.reset();
        view_pull_        = {};
        peer_token_       = std::move(peer_tok);
        tracking_peer_    = true;
        storage_ = storage::bound_live;
    }

    // owned_value 상태로 전환 (copy).
    // 이미 owned_value 상태라면 optional은 in-place 갱신되므로
    // 주소가 유지되어 token_을 무효화하지 않아도 됩니다.
    void assign_value_unlocked_(const OwnedT& v) {
        revoke_owned_token_if_needed_();   // 다른 상태에서 전환하는 경우 대비
        bound_ptr_        = nullptr;
        owned_value_      = v;
        stored_ptr_value_ = nullptr;
        view_obj_.reset();
        view_pull_        = {};
        peer_token_.reset();
        tracking_peer_    = false;
        storage_ = storage::owned_value;
    }

    // owned_value 상태로 전환 (move)
    void assign_value_unlocked_(OwnedT&& v)
        noexcept(std::is_nothrow_move_assignable_v<OwnedT>)
    {
        revoke_owned_token_if_needed_();
        bound_ptr_        = nullptr;
        owned_value_      = std::move(v);
        stored_ptr_value_ = nullptr;
        view_obj_.reset();
        view_pull_        = {};
        peer_token_.reset();
        tracking_peer_    = false;
        storage_ = storage::owned_value;
    }

    void assign_must_copy_unlocked_(RefT* p) noexcept {
        revoke_owned_token_if_needed_();
        bound_ptr_        = nullptr;
        owned_value_.reset();
        stored_ptr_value_ = p;
        view_obj_.reset();
        view_pull_        = {};
        peer_token_.reset();
        tracking_peer_    = false;
        storage_ = storage::stored_pointer;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // peer 만료 검사 + 예외 (락 보유 상태에서 호출)
    //
    // tracking_peer_=true인 bound_live 상태에서 peer가 만료됐으면 throw.
    // get()과 복합 대입의 진입부에서 호출합니다.
    // ─────────────────────────────────────────────────────────────────────────
    void throw_if_peer_expired_() const {
        if (storage_ == storage::bound_live
            && tracking_peer_
            && peer_token_.expired())
        {
            throw std::logic_error(
                "nref: 바인딩된 peer nref가 이미 소멸했습니다. "
                "소멸 전에 detach()를 호출해 값을 보존하세요.");
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 읽기 경로 (락 보유 상태에서 호출)
    // ─────────────────────────────────────────────────────────────────────────
    [[nodiscard]] OwnedT read_snapshot_unlocked_() const {
        throw_if_peer_expired_();
        switch (storage_) {
        case storage::bound_live:
            return static_cast<OwnedT>(*bound_ptr_);

        case storage::owned_value:
            return *owned_value_;

        case storage::stored_pointer:
            throw std::logic_error(
                "nref::get(): stored_pointer 상태는 get()을 지원하지 않습니다. "
                "get_address()를 사용하세요.");

        case storage::bound_view:
            return view_pull_ ? view_pull_() : OwnedT{};

        default:
            return OwnedT{};
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 쓰기 경로 (락 보유 상태에서 호출)
    // ─────────────────────────────────────────────────────────────────────────
    template<class Fn>
    NRX_FORCE_INLINE void apply_op_unlocked_(Fn&& fn) {
        throw_if_peer_expired_();
        switch (storage_) {
        case storage::bound_live:
            fn(*bound_ptr_);
            break;
        case storage::owned_value:
            fn(owned_value_.value());
            break;
        case storage::empty:
            owned_value_ = OwnedT{};
            storage_     = storage::owned_value;
            fn(owned_value_.value());
            break;
        case storage::stored_pointer:
            throw std::logic_error(
                "nref: stored_pointer 상태에서 복합 대입은 지원하지 않습니다.");
        case storage::bound_view:
            throw std::logic_error(
                "nref: bound_view 상태에서 복합 대입은 지원하지 않습니다.");
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // nref* 대입 시 소스 상태 복사 (dst/src 락을 호출자가 보유한 상태에서 호출)
    // ─────────────────────────────────────────────────────────────────────────
    template<typename R, typename O>
    static void copy_internal_state_from_(nref& dst, nref<R, O>& src) {
        using SS = typename nref<R, O>::storage;

        if (src.storage_ == SS::bound_live && src.bound_ptr_) {
            if (src.tracking_peer_) {
                // 소스가 peer를 추적 중이면 추적 정보를 그대로 전파합니다.
                // b = &a처럼 체이닝되는 경우에도 peer 소멸 감지가 유지됩니다.
                auto tok = src.peer_token_.lock();
                if (!tok) {
                    // 복사 시점에 이미 peer가 소멸 — 즉시 예외
                    throw std::logic_error(
                        "nref: 복사 대상의 peer nref가 이미 소멸했습니다. "
                        "소멸 전에 detach()를 호출해 값을 보존하세요.");
                }
                dst.assign_ptr_with_peer_unlocked_(src.bound_ptr_, std::move(tok));
            } else {
                // 단순 외부 변수 바인딩(&x) — peer 추적 없이 포인터만 복사
                dst.assign_ptr_unlocked_(src.bound_ptr_);
            }
            return;
        }
        if (src.storage_ == SS::owned_value && src.owned_value_) {
            dst.assign_ptr_with_peer_unlocked_(
                &src.owned_value_.value(),
                src.lifetime_token_unlocked_());
            return;
        }
        if (src.storage_ == SS::stored_pointer && src.stored_ptr_value_) {
            dst.assign_must_copy_unlocked_(src.stored_ptr_value_);
            return;
        }
        if (src.storage_ == SS::bound_view && src.view_pull_) {
            dst.assign_value_unlocked_(src.view_pull_());
            return;
        }
        dst.reset_unlocked_();
    }

    [[nodiscard]] std::shared_ptr<void> lifetime_token_unlocked_() const {
        if (!token_) token_ = std::make_shared<int>(0);
        return token_;
    }

public:
    // =========================================================================
    // 생성자
    // =========================================================================

    nref() noexcept = default;

    nref(RefT* p)
    { assign_ptr_unlocked_(p); NRX_ASSERT(check_invariant_()); }

    nref(const OwnedT& v)
    { assign_value_unlocked_(v); NRX_ASSERT(check_invariant_()); }

    nref(OwnedT&& v)
        noexcept(std::is_nothrow_move_constructible_v<OwnedT>)
    { assign_value_unlocked_(std::move(v)); NRX_ASSERT(check_invariant_()); }

    nref(copy_address_request<RefT> r) noexcept
    { assign_must_copy_unlocked_(r.ptr_); NRX_ASSERT(check_invariant_()); }

    // input range → 첫 원소를 owned_value로 스냅샷
    template<std::ranges::input_range R>
        requires (!std::is_pointer_v<std::ranges::range_value_t<R>>&&
    std::convertible_to<std::ranges::range_value_t<R>, OwnedT>)
        nref(R&& r) {
        auto it = std::ranges::begin(r);
        if (it != std::ranges::end(r)) assign_value_unlocked_(static_cast<OwnedT>(*it));
        NRX_ASSERT(check_invariant_());
    }

    // first_of_range{r} → bound_view
    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_value_t<R>, OwnedT>
    nref(first_of_range<R>&& fr) { *this = std::move(fr); NRX_ASSERT(check_invariant_()); }

    // snap_range{r} → owned_value (첫 원소 즉시 스냅샷)
    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_value_t<R>, OwnedT>
    nref(snap_range<R>&& sr) { *this = std::move(sr); NRX_ASSERT(check_invariant_()); }


    // =========================================================================
    // Rule of 5
    // =========================================================================

    nref(const nref& o) {
        std::shared_lock lock(o.mtx_);
        bound_ptr_        = o.bound_ptr_;
        owned_value_      = o.owned_value_;
        stored_ptr_value_ = o.stored_ptr_value_;
        view_obj_         = o.view_obj_;
        view_pull_        = o.view_pull_;
        storage_          = o.storage_;
        peer_token_       = o.peer_token_;
        tracking_peer_    = o.tracking_peer_;
        NRX_ASSERT(check_invariant_());
    }

    nref(nref&& o) noexcept {
        std::unique_lock lock(o.mtx_);
        // owned_value를 이동하기 전에 source의 token_을 먼저 무효화합니다.
        // 이렇게 해야 기존 peer들이 weak_ptr 만료를 즉시 감지합니다.
        // destination은 새 주소에 owned_value_를 갖게 되므로
        // token_은 null로 시작하고 필요 시 lifetime_token()에서 재발급됩니다.
        o.revoke_owned_token_if_needed_();
        bound_ptr_        = o.bound_ptr_;
        owned_value_      = std::move(o.owned_value_);
        stored_ptr_value_ = o.stored_ptr_value_;
        view_obj_         = std::move(o.view_obj_);
        view_pull_        = std::move(o.view_pull_);
        storage_          = o.storage_;
        peer_token_       = std::move(o.peer_token_);
        tracking_peer_    = o.tracking_peer_;
        // token_은 이전하지 않음 — null(lazy)로 시작
        o.reset_unlocked_();
        NRX_ASSERT(check_invariant_());
    }

    nref& operator=(const nref& o) {
        if (this == &o) return *this;
        std::scoped_lock lock(mtx_, o.mtx_);
        bound_ptr_        = o.bound_ptr_;
        owned_value_      = o.owned_value_;
        stored_ptr_value_ = o.stored_ptr_value_;
        view_obj_         = o.view_obj_;
        view_pull_        = o.view_pull_;
        storage_          = o.storage_;
        peer_token_       = o.peer_token_;
        tracking_peer_    = o.tracking_peer_;
        token_.reset();
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    nref& operator=(nref&& o) noexcept {
        if (this == &o) return *this;
        std::scoped_lock lock(mtx_, o.mtx_);
        // destination이 owned_value를 버리는 경우에도 token_ 무효화
        revoke_owned_token_if_needed_();
        // source의 owned_value를 이동하기 전에 source token_ 무효화
        o.revoke_owned_token_if_needed_();
        bound_ptr_        = o.bound_ptr_;
        owned_value_      = std::move(o.owned_value_);
        stored_ptr_value_ = o.stored_ptr_value_;
        view_obj_         = std::move(o.view_obj_);
        view_pull_        = std::move(o.view_pull_);
        storage_          = o.storage_;
        peer_token_       = std::move(o.peer_token_);
        tracking_peer_    = o.tracking_peer_;
        // token_은 이전하지 않음 — null(lazy)로 시작
        token_.reset();
        o.reset_unlocked_();
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    ~nref() noexcept = default;

    // =========================================================================
    // 상태 조회
    // =========================================================================

    [[nodiscard]] bool is_empty()      const noexcept { std::shared_lock l(mtx_); return storage_ == storage::empty;          }
    [[nodiscard]] bool is_bound_live() const noexcept { std::shared_lock l(mtx_); return storage_ == storage::bound_live;     }
    [[nodiscard]] bool is_owned()      const noexcept { std::shared_lock l(mtx_); return storage_ == storage::owned_value;    }
    [[nodiscard]] bool is_const_ptr()  const noexcept { std::shared_lock l(mtx_); return storage_ == storage::stored_pointer; }
    [[nodiscard]] bool is_bound_view() const noexcept { std::shared_lock l(mtx_); return storage_ == storage::bound_view;     }

    [[nodiscard]] storage current_storage() const noexcept { std::shared_lock l(mtx_); return storage_; }

    // peer가 이미 소멸했는지 미리 확인합니다.
    // get()을 호출하기 전에 체크하고 싶을 때 사용합니다.
    [[nodiscard]] bool is_peer_expired() const noexcept {
        std::shared_lock l(mtx_);
        return storage_ == storage::bound_live
            && tracking_peer_
            && peer_token_.expired();
    }

    // =========================================================================
    // reset / detach
    // =========================================================================

    void reset() noexcept {
        std::unique_lock lock(mtx_);
        reset_unlocked_();
        NRX_ASSERT(check_invariant_());
    }

    // bound_live 상태에서 현재 값을 스냅샷하여 owned_value로 전환합니다.
    // peer 소멸 전에 값을 보존하려면 명시적으로 이 함수를 호출하세요.
    // peer가 이미 소멸한 상태에서 호출하면 std::logic_error를 던집니다.
    void detach() {
        std::unique_lock lock(mtx_);
        if (storage_ != storage::bound_live) return;
        throw_if_peer_expired_();
        OwnedT snap    = static_cast<OwnedT>(*bound_ptr_);
        bound_ptr_     = nullptr;
        peer_token_.reset();
        tracking_peer_ = false;
        owned_value_   = std::move(snap);
        storage_       = storage::owned_value;
        NRX_ASSERT(check_invariant_());
    }

    // =========================================================================
    // 대입 연산자
    // =========================================================================

    nref& operator=(RefT* p) {
        std::unique_lock lock(mtx_);
        assign_ptr_unlocked_(p);
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    nref& operator=(const OwnedT& v)
        noexcept(std::is_nothrow_copy_assignable_v<OwnedT>)
    {
        std::unique_lock lock(mtx_);
        throw_if_peer_expired_();
        switch (storage_) {
        case storage::bound_live:
            *bound_ptr_ = static_cast<RefT>(v);
            break;
        case storage::owned_value:
            owned_value_ = v;
            break;
        case storage::empty:
            assign_value_unlocked_(v);
            break;
        case storage::stored_pointer:
            throw std::logic_error(
                "nref: stored_pointer 상태에서 값 대입은 지원하지 않습니다. "
                "상태를 먼저 전환하세요.");
        case storage::bound_view:
            throw std::logic_error(
                "nref: bound_view 상태에서 값 대입은 지원하지 않습니다. "
                "상태를 먼저 전환하세요.");
        }
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    nref& operator=(OwnedT&& v)
        noexcept(std::is_nothrow_move_assignable_v<OwnedT>)
    {
        std::unique_lock lock(mtx_);
        throw_if_peer_expired_();
        switch (storage_) {
        case storage::bound_live:
            *bound_ptr_ = static_cast<RefT>(v);
            break;
        case storage::owned_value:
            owned_value_ = std::move(v);
            break;
        case storage::empty:
            assign_value_unlocked_(std::move(v));
            break;
        case storage::stored_pointer:
            throw std::logic_error(
                "nref: stored_pointer 상태에서 값 대입은 지원하지 않습니다. "
                "상태를 먼저 전환하세요.");
        case storage::bound_view:
            throw std::logic_error(
                "nref: bound_view 상태에서 값 대입은 지원하지 않습니다. "
                "상태를 먼저 전환하세요.");
        }
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    nref& operator=(copy_address_request<RefT> r) noexcept {
        std::unique_lock lock(mtx_);
        assign_must_copy_unlocked_(r.ptr_);
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    nref& operator=(std::nullopt_t) noexcept { reset(); return *this; }

    template<std::ranges::input_range R>
    requires (!std::is_pointer_v<std::ranges::range_value_t<R>> &&
              std::convertible_to<std::ranges::range_value_t<R>, OwnedT>)
    nref& operator=(R&& r) {
        using V = std::decay_t<R>;
        return (*this = first_of_range<V>{ V(std::forward<R>(r)) });
    }

    template<std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_value_t<R>, OwnedT>
    nref& operator=(first_of_range<R>&& fr) {
        std::unique_lock lock(mtx_);
        revoke_owned_token_if_needed_();
        using V     = std::decay_t<R>;
        auto holder = std::make_shared<V>(std::move(fr.r_));
        view_pull_ = [holder]() -> OwnedT {
            auto it = std::ranges::begin(*holder);
            if (it == std::ranges::end(*holder)) return OwnedT{};
            return static_cast<OwnedT>(*it);
        };
        view_obj_         = holder;
        bound_ptr_        = nullptr;
        owned_value_.reset();
        stored_ptr_value_ = nullptr;
        peer_token_.reset();
        tracking_peer_    = false;
        storage_ = storage::bound_view;
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    template<std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_value_t<R>, OwnedT>
    nref& operator=(snap_range<R>&& sr) {
        auto it = std::ranges::begin(sr.r_);
        if (it != std::ranges::end(sr.r_)) return (*this = static_cast<OwnedT>(*it));
        reset();
        return *this;
    }

    template<class R, class O>
    nref& operator=(nref<R, O>* p) requires std::same_as<R, RefT> {
        if (!p) { reset(); return *this; }
        if (static_cast<void*>(this) == static_cast<void*>(p)) return *this;
        std::scoped_lock both(mtx_, p->mtx_);
        copy_internal_state_from_(*this, *p);
        NRX_ASSERT(check_invariant_());
        return *this;
    }

    // =========================================================================
    // 복합 대입
    // peer가 소멸한 상태에서 호출하면 std::logic_error를 던집니다.
    // =========================================================================

    template<class U>
    requires requires(RefT& r, OwnedT& o, const U& u) { r += u; o += u; }
    nref& operator+=(const U& d) {
        std::unique_lock l(mtx_); apply_op_unlocked_([&](auto& x){ x += d; });
        NRX_ASSERT(check_invariant_()); return *this;
    }

    template<class U>
    requires requires(RefT& r, OwnedT& o, const U& u) { r -= u; o -= u; }
    nref& operator-=(const U& d) {
        std::unique_lock l(mtx_); apply_op_unlocked_([&](auto& x){ x -= d; });
        NRX_ASSERT(check_invariant_()); return *this;
    }

    template<class U>
    requires requires(RefT& r, OwnedT& o, const U& u) { r *= u; o *= u; }
    nref& operator*=(const U& d) {
        std::unique_lock l(mtx_); apply_op_unlocked_([&](auto& x){ x *= d; });
        NRX_ASSERT(check_invariant_()); return *this;
    }

    template<class U>
    requires requires(RefT& r, OwnedT& o, const U& u) { r /= u; o /= u; }
    nref& operator/=(const U& d) {
        std::unique_lock l(mtx_); apply_op_unlocked_([&](auto& x){ x /= d; });
        NRX_ASSERT(check_invariant_()); return *this;
    }

    // =========================================================================
    // 접근자
    // =========================================================================

    // 현재 값을 반환합니다.
    // 다음 경우 std::logic_error를 던집니다:
    //   - stored_pointer 상태 (get_address() 사용)
    //   - peer가 소멸한 bound_live 상태 (detach() 또는 is_peer_expired() 사용)
    [[nodiscard]] OwnedT get() const {
        std::shared_lock l(mtx_);
        return read_snapshot_unlocked_();
    }

    [[nodiscard]] RefT* get_address() noexcept {
        std::shared_lock l(mtx_);
        NRX_ASSERT(storage_ == storage::stored_pointer);
        return stored_ptr_value_;
    }

    [[nodiscard]] const RefT* get_address() const noexcept {
        std::shared_lock l(mtx_);
        NRX_ASSERT(storage_ == storage::stored_pointer);
        return stored_ptr_value_;
    }

    [[nodiscard]] std::shared_ptr<void> lifetime_token() const {
        std::unique_lock l(mtx_);
        return lifetime_token_unlocked_();
    }
};

} // namespace nrx::ref

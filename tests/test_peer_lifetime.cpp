/**
 * @file nref_test.cpp
 * @brief nref 테스트 스위트
 *
 * 빌드:
 *   g++ -std=c++20 -fsanitize=address,undefined -pthread nref_test.cpp -o nref_test
 *   ./nref_test
 *
 * TSan(스레드 sanitizer) 별도 빌드:
 *   g++ -std=c++20 -fsanitize=thread -pthread nref_test.cpp -o nref_test_tsan
 */
#pragma once
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>
#include <ranges>

#include "../include/nrx/ref/nref.hpp"
#include "../include/nrx/ref/nref_dsl.hpp"


using namespace nrx::ref;
using namespace nrx::ref::dsl;

// ─────────────────────────────────────────────────────────────────────────────
// 테스트 프레임워크 (단순 매크로)
// ─────────────────────────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

#define TEST(name) void name()
#define RUN(name)  do { \
    try { name(); std::cout << "[PASS] " #name "\n"; ++g_pass; } \
    catch (const std::exception& e) { std::cout << "[FAIL] " #name " — " << e.what() << "\n"; ++g_fail; } \
    catch (...) { std::cout << "[FAIL] " #name " — unknown exception\n"; ++g_fail; } \
} while(0)

#define REQUIRE(cond) do { if (!(cond)) throw std::runtime_error("REQUIRE failed: " #cond); } while(0)
#define REQUIRE_THROWS(expr) do { \
    bool threw = false; \
    try { (expr); } catch (const std::logic_error&) { threw = true; } \
    if (!threw) throw std::runtime_error("REQUIRE_THROWS failed: " #expr); \
} while(0)

// ─────────────────────────────────────────────────────────────────────────────
// 1. 기본 상태 전환
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_empty_default) {
    nref<int> r;
    REQUIRE(r.is_empty());
    REQUIRE(r.current_storage() == nref<int>::storage::empty);
}

TEST(test_bound_live_basic) {
    int x = 10;
    nref<int> r = &x;
    REQUIRE(r.is_bound_live());
    REQUIRE(r.get() == 10);

    x = 42;
    REQUIRE(r.get() == 42);   // 원본 변경 반영
}

TEST(test_bound_live_write) {
    int x = 3;
    nref<int> r = &x;
    r += 2;
    REQUIRE(x == 5);          // 원본에 직접 씀
    REQUIRE(r.get() == 5);
}

TEST(test_owned_value_basic) {
    nref<int> r = 99;
    REQUIRE(r.is_owned());
    REQUIRE(r.get() == 99);

    r = 100;
    REQUIRE(r.get() == 100);
}

TEST(test_stored_pointer_basic) {
    int x = 7;
    nref<int> r = must_copy(&x);
    REQUIRE(r.is_const_ptr());
    REQUIRE(r.get_address() == &x);
    REQUIRE(*r.get_address() == 7);
}

TEST(test_stored_pointer_get_throws) {
    int x = 7;
    nref<int> r = must_copy(&x);
    REQUIRE_THROWS(r.get());
}

TEST(test_nullopt_reset) {
    nref<int> r = 42;
    r = std::nullopt;
    REQUIRE(r.is_empty());

    r.reset();
    REQUIRE(r.is_empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. bound_view / DSL
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_first_of_range_basic) {
    using NR = nref<int>;
    std::vector<int> v{ 10, 20, 30 };
    NR r;
    r = NR::first_of_range<std::vector<int>>{ std::move(v) };
    REQUIRE(r.is_bound_view());
    REQUIRE(r.get() == 10);
}

TEST(test_snap_range_basic) {
    using NR = nref<int>;
    std::vector<int> v{ 42, 43 };
    NR r;
    r = NR::snap_range<std::vector<int>>{ std::move(v) };
    REQUIRE(r.is_owned());
    REQUIRE(r.get() == 42);
}

TEST(test_dsl_pipe_snapshot) {
    int v = 3;
    nref<int> r =
        v 
        | std::views::transform([](int x) { return x * 10; });
    REQUIRE(r.get() == 30);
}

TEST(test_dsl_pipe_live) {
    int v = 2;
    nref<int> r;
    r = &v
        | deref
        | std::views::transform([](int x) { return x * 10; });

    v = 5;
    REQUIRE(r.get() == 50);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. stored_pointer / bound_view 에서 값 대입 throw
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_stored_pointer_value_assign_throws) {
     int x = 1;
     nref<int> r = must_copy(&x);
     REQUIRE_THROWS(r = 3);
     REQUIRE_THROWS(r += 1);
}

TEST(test_bound_view_value_assign_throws) {
    using NR = nref<int>;
    std::vector<int> v{ 1,2,3 };
    NR r;
    r = NR::first_of_range<std::vector<int>>{ std::move(v) };
    REQUIRE_THROWS(r = 42);
    REQUIRE_THROWS(r += 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. nullptr 바인딩 → empty
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_nullptr_becomes_empty) {
    nref<int> r((int*)nullptr);
    REQUIRE(r.is_empty());

    r = (int*)nullptr;
    REQUIRE(r.is_empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. detach()
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_explicit_detach) {
    int x = 77;
    nref<int> r = &x;
    r.detach();
    x = 999;
    REQUIRE(r.is_owned());
    REQUIRE(r.get() == 77);   // 원본 변경과 무관
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. peer 수명 추적 — 핵심 버그 시나리오들
// ─────────────────────────────────────────────────────────────────────────────

// 기본: peer 소멸 후 get() → throw
TEST(test_peer_expired_throws) {
    nref<int> dst;
    {
        nref<int> src{ 42 };
        dst = &src;
        REQUIRE(dst.get() == 42);
    }
    REQUIRE(dst.is_peer_expired());
    REQUIRE_THROWS(dst.get());
}

// peer 소멸 후 복합 대입 → throw
TEST(test_peer_expired_compound_assign_throws) {
    nref<int> dst;
    {
        nref<int> src{ 10 };
        dst = &src;
    }
    REQUIRE_THROWS(dst += 1);
}

// peer 소멸 후 detach → throw
TEST(test_peer_expired_detach_throws) {
    nref<int> dst;
    {
        nref<int> src{ 10 };
        dst = &src;
    }
    REQUIRE_THROWS(dst.detach());
}

// b = &a 체이닝 — peer 추적 전파 (GPT/Gemini가 지적한 버그)
TEST(test_peer_chain_propagation) {
    nref<int> c;
    {
        nref<int> owner{ 123 };
        nref<int> a;
        a = &owner;           // a → owner.owned_value_, peer=owner.token_

        nref<int> b;
        b = &a;               // b도 같은 peer를 추적해야 함

        c = &b;               // c도 마찬가지

        REQUIRE(c.get() == 123);
    }
    // owner, a, b 소멸
    REQUIRE(c.is_peer_expired());
    REQUIRE_THROWS(c.get());
}

// b = &a 시점에 이미 peer 소멸 → 대입 즉시 throw
TEST(test_peer_already_expired_at_copy_time) {
    nref<int> a;
    {
        nref<int> owner{ 42 };
        a = &owner;
    }
    nref<int> b;
    REQUIRE_THROWS(b = &a);   // 대입 시점에 이미 소멸
}

// owner가 owned_value를 버릴 때 token_ 무효화
TEST(test_token_revoke_on_owned_exit) {
    int x = 3;
    nref<int> owner{ 1 };
    nref<int> a;
    a = &owner;               // a → owner.owned_value_, peer=owner.token_

    owner = &x;               // owner: owned_value → bound_live, token_ 무효화

    REQUIRE(a.is_peer_expired());
    REQUIRE_THROWS(a.get());
}

// owner move 후 peer 만료 확인
TEST(test_token_revoke_on_move) {
    nref<int> a;
    {
        nref<int> owner{ 99 };
        a = &owner;
        REQUIRE(a.get() == 99);

        nref<int> new_owner = std::move(owner);  // move 시 token_ 무효화
        REQUIRE(a.is_peer_expired());
        REQUIRE_THROWS(a.get());
    }
}

// move 대입도 동일
TEST(test_token_revoke_on_move_assign) {
    nref<int> a;
    nref<int> new_owner;
    {
        nref<int> owner{ 55 };
        a = &owner;
        new_owner = std::move(owner);
    }
    REQUIRE(a.is_peer_expired());
    REQUIRE_THROWS(a.get());
}

// make_bad() 시나리오 — 원래 제시된 예제
TEST(test_make_bad_scenario) {
    auto make_bad = []() -> nref<int> {
        nref<int> owner{ 123 };
        nref<int> a;
        a = &owner;
        nref<int> b;
        b = &a;
        return b;
        };

    auto x = make_bad();
    REQUIRE(x.is_peer_expired());
    REQUIRE_THROWS(x.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Rule of 5 — 복사/이동 후 독립성
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_copy_independence) {
    nref<int> a = 10;
    nref<int> b = a;
    b = 20;
    REQUIRE(a.get() == 10);   // a는 영향받지 않음
    REQUIRE(b.get() == 20);
}

TEST(test_move_leaves_source_empty) {
    nref<int> a = 42;
    nref<int> b = std::move(a);
    REQUIRE(a.is_empty());
    REQUIRE(b.get() == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. nref* 대입 — 소스 상태별 파생
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_nref_ptr_from_bound_live) {
    int x = 10;
    nref<int> src = &x;
    nref<int> dst;
    dst = &src;
    x = 20;
    REQUIRE(dst.get() == 20);  // 같은 외부 변수 추종
}

TEST(test_nref_ptr_from_owned) {
    nref<int> src{ 123 };
    nref<int> dst;
    dst = &src;
    REQUIRE(dst.get() == 123);
    REQUIRE(dst.is_bound_live());
}

TEST(test_nref_ptr_from_view_snaps) {
    using NR = nref<int>;
    NR src;
    src = NR::first_of_range<std::vector<int>>{ {1,2,3} };
    NR dst;
    dst = &src;
    REQUIRE(dst.is_owned());   // view는 스냅샷으로 변환
    REQUIRE(dst.get() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. std::string 타입 (non-trivial OwnedT)
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_string_bound_live) {
    std::string s = "hello";
    nref<std::string> r = &s;
    REQUIRE(r.get() == "hello");

    s = "world";
    REQUIRE(r.get() == "world");
}

TEST(test_string_peer_expired_throws) {
    nref<std::string> dst;
    {
        nref<std::string> src{ std::string("test") };
        dst = &src;
        REQUIRE(dst.get() == "test");
    }
    REQUIRE_THROWS(dst.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. 멀티스레드 — 읽기 병렬성 (TSan으로 검증)
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_concurrent_reads) {
    int x = 42;
    nref<int> r = &x;

    std::vector<std::thread> threads;
    std::vector<int> results(8);

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&r, &results, i]() {
            results[i] = r.get();
            });
    }
    for (auto& t : threads) t.join();

    for (int v : results) REQUIRE(v == 42);
}

TEST(test_concurrent_write_and_read) {
    nref<int> r = 0;

    std::thread writer([&r]() {
        for (int i = 0; i < 1000; ++i) r = i;
        });
    std::thread reader([&r]() {
        for (int i = 0; i < 1000; ++i) {
            try { r.get(); }
            catch (...) {}
        }
        });

    writer.join();
    reader.join();
    // 크래시/TSan 에러 없으면 통과
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // 1. 기본 상태 전환
    RUN(test_empty_default);
    RUN(test_bound_live_basic);
    RUN(test_bound_live_write);
    RUN(test_owned_value_basic);
    RUN(test_stored_pointer_basic);
    RUN(test_stored_pointer_get_throws);
    RUN(test_nullopt_reset);

    // 2. bound_view / DSL
    RUN(test_first_of_range_basic);
    RUN(test_snap_range_basic);
    RUN(test_dsl_pipe_snapshot);
    RUN(test_dsl_pipe_live);

    // 3. 잘못된 상태에서 값 대입 throw
    /*
    RUN(test_stored_pointer_value_assign_throws);
    RUN(test_bound_view_value_assign_throws);
    */

    // 4. nullptr 바인딩
    RUN(test_nullptr_becomes_empty);

    // 5. detach
    RUN(test_explicit_detach);

    // 6. peer 수명 추적 — 핵심 버그 시나리오
    /*
    RUN(test_peer_expired_throws);
    RUN(test_peer_expired_compound_assign_throws);
    RUN(test_peer_expired_detach_throws);
    */
    RUN(test_peer_chain_propagation);
    RUN(test_peer_already_expired_at_copy_time);
    RUN(test_token_revoke_on_owned_exit);
    RUN(test_token_revoke_on_move);
    RUN(test_token_revoke_on_move_assign);
    RUN(test_make_bad_scenario);

    // 7. Rule of 5
    RUN(test_copy_independence);
    RUN(test_move_leaves_source_empty);

    // 8. nref* 대입
    RUN(test_nref_ptr_from_bound_live);
    RUN(test_nref_ptr_from_owned);
    RUN(test_nref_ptr_from_view_snaps);

    // 9. std::string
    RUN(test_string_bound_live);
    RUN(test_string_peer_expired_throws);

    // 10. 멀티스레드
    RUN(test_concurrent_reads);
    RUN(test_concurrent_write_and_read);

    std::cout << "\n" << g_pass << " passed, " << g_fail << " failed.\n";
    return g_fail > 0 ? 1 : 0;
}

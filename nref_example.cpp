#include <iostream>

#include "nrx/ref/nref.hpp"
#include "nrx/ref/nref_dsl.hpp"

int main() {
    using nr = nrx::ref::nref<int>;
    using nrx::ref::dsl::deref;

    int x = 10;

    nr bound{ &x };
    nr owned{ 123 };
    nr stored;
    stored = nrx::ref::must_copy(&x);

    std::cout << "bound = " << bound.get() << "\n";
    std::cout << "owned = " << owned.get() << "\n";

    bound += 5;
    std::cout << "x after bound += 5 : " << x << "\n";

    int* p = stored.get_address();
    std::cout << "stored ptr value = " << (p ? *p : -1) << "\n";

    for (int v : &bound | deref) {
        std::cout << "pipe value = " << v << "\n";
    }

    return 0;
}

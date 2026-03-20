# nref
## 경고  
아직 완전한 정비 전입니다.

## 기능
nref는 세 개 모드를 왔다갔다하며 유연한 레퍼런스 기능을 제공합니다.
bound_view 모드에서는 편리한 다른 기능을 제공합니다.  
### null이 가능합니다.  
std::nullopt를 대입하거나 reset 메서드를 호출하면 std::nullopt로 전환됩니다.  
이름은 nullable_reference의 준말입니다.  

## 모드 종류
bound_live: 외부 변수 참조  
owned_value: 자체 값 소유  
stored_pointer: 명시적 포인터 저장  
bound_view : 밑에서 설명 

## 모드에 따른 차이

### bound_live
일반적인 레퍼런스처럼 외부 변수에 바인딩하여 사용합니다. 바인딩된 원본 값이 바뀌면 nref의 값도 동일하게 바뀝니다. 

### owned_value
외부 포인터를 참조하지 않고, 객체 자체가 값을 직접 소유합니다.

### stored_pointer 
포인터의 메모리 주소 자체를 보관해야 할 때 사용합니다. 의도치 않은 참조로 인한 버그를 막기 위해 반드시 must_copy()를 통해 명시적으로 전달해야 합니다.  
이 때 값을 얻으려면 get이 아닌 get_address 함수를 써야함.

## bound_view 모드
더 많이 다뤘습니다. 예제를 보시면 이해될 겁니다

### snapshot된 값을 단순 변환하는 예제
```cpp  
#include "nref.hpp"  
#include "nref_dsl.hpp"  
  
using nrx::ref::nref;  
using namespace nrx::ref::dsl;  
   
int main() {  
    int v = 2;
    nref<int> r =   
      v | std::views::transform([](int x) { return x * 10; });  
  
    std::cout << r.get() << '\n'; // 20  
}  
```

### 변수를 레퍼런싱해서 추종하는 예제
```cpp
#include "nref.hpp"  
#include "nref_dsl.hpp"  
  
using nrx::ref::nref;  
using namespace nrx::ref::dsl;  
  
int main() {  
    int v = 2;
  
    nref<int> r;  
    r = &v  
      | deref  //앞에 온 게 주소라면 반드시 이게 들어가야합니다
      | std::views::transform([](int x) { return x * 10; });  

    v = 3;
    std::cout << r.get() << '\n'; // 30
}  
```

### 

## 모드 변환
### &변수를 넣으면 bound_live  
```cpp  
int x = 10;  
nrx::ref::nref<int> a;  
a = &x;   // empty -> bound_live
```

### 값을 넣으면 owned_value  
```cpp  
nrx::ref::nref<int> a;  
a = 123;  // empty -> owned_value
```

### must_copy(&x)를 쓰면 stored_pointer  
```cpp  
int x = 10;  
nrx::ref::nref<int> a;  
a = nrx::ref::must_copy(&x);  // -> stored_pointer
```

### reset()이나 std::nullopt는 empty  
```cpp  
nrx::ref::nref<int> a{123};  
  
a.reset();        // -> empty  
// 또는  
a = std::nullopt; // -> empty  
```

### 다른 nref를 포인터로 넘기면 원본 상태에 따라 다름  
```cpp  
using NR = nrx::ref::nref<int>;  
int x = 10;  
  
NR s1{&x};  
NR d1;  
d1 = &s1;   // d1 -> bound_live  
  
NR s2{123};  
NR d2;  
d2 = &s2;   // d2도 owned가 아니라 bound_live  
  
NR s3;  
s3 = NR::live_range<std::vector<int>>{{1,2,3}};  
NR d3;  
d3 = &s3;   // d3 -> owned_value  
```

### bound_live로 참조한 변수의 수명이 다했을 때 bound_live -> owned_value  
```cpp  
using NR = nrx::ref::nref<int>;  
  
NR src{123};  
NR dst;  
dst = &src;                 // dst는 src 내부값 쪽에 bound_live  
dst.enable_auto_detach(true);  
// src가 먼저 사라진 뒤  
dst += 1;                   // 구현상 여기서 owned_value로 전환 시도
```


#include "share/text_analysis/ob_token_stream.h"
#include "lib/charset/ob_charset.h"
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using namespace oceanbase::common;
using namespace oceanbase::share;
class TrackingAllocator final : public ObIAllocator {
public:
  std::vector<std::pair<char *,int64_t>> blocks;
  bool fail=false;
  ~TrackingAllocator() { for(auto p:blocks)std::free(p.first); }
  void *alloc(const int64_t size) override {
    if(fail)return nullptr;
    auto p=static_cast<char *>(std::malloc(size));
    if(p)blocks.emplace_back(p,size);
    return p;
  }
  void *alloc(const int64_t size,const ObMemAttr &) override { return alloc(size); }
  void free(void *) override {} // Retain until test arena lifetime ends.
  bool owns(const char *p) const {
    const auto value=reinterpret_cast<uintptr_t>(p);
    for(auto b:blocks) { const auto begin=reinterpret_cast<uintptr_t>(b.first);if(value>=begin && value-begin<static_cast<uintptr_t>(b.second))return true; }
    return false;
  }
};
template<class T> auto configure(T &n, ObIAllocator &a,int) -> decltype(n.set_output_allocator(a),void()) { n.set_output_allocator(a); }
template<class T> void configure(T &,ObIAllocator &,long) {} // Old code exercises the same real normalizer.
#define CHECK(x) do { if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<'\n';return 1;} }while(0)
int main() {
  ObCharset::init_charset();
  auto cs=ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  std::string text="HELLO WORLD Résumé";
  ObDatum document;document.set_string(text.data(),text.size());
  ObTextWhitespaceTokenizer tokenizer;
  CHECK(tokenizer.open(document,cs)==0);
  TrackingAllocator arena;
  ObBasicEnglishNormalizer normalizer;
  CHECK(normalizer.init(cs,tokenizer)==0);
  configure(normalizer,arena,0);
  ObDatum first,second,third;int64_t frequency=0;
  CHECK(normalizer.get_next(first,frequency)==0 && frequency==1);
  CHECK(std::string(first.ptr_,first.len_)=="hello");
  CHECK(arena.owns(first.ptr_));
  CHECK(normalizer.get_next(second,frequency)==0);
  CHECK(arena.owns(second.ptr_) && std::string(second.ptr_,second.len_)=="world");
  CHECK(std::string(first.ptr_,first.len_)=="hello");
  CHECK(normalizer.get_next(third,frequency)==0);
  CHECK(arena.owns(third.ptr_) && std::string(third.ptr_,third.len_)=="résumé");
  normalizer.reuse();normalizer.reset();
  CHECK(std::string(first.ptr_,first.len_)=="hello");
  CHECK(std::string(second.ptr_,second.len_)=="world");
  CHECK(std::string(third.ptr_,third.len_)=="résumé");
  ObTextWhitespaceTokenizer default_input;CHECK(default_input.open(document,cs)==0);
  ObBasicEnglishNormalizer default_normalizer;CHECK(default_normalizer.init(cs,default_input)==0);
  ObDatum default_token;CHECK(default_normalizer.get_next(default_token,frequency)==0);
  CHECK(std::string(default_token.ptr_,default_token.len_)=="hello" && !arena.owns(default_token.ptr_));
  ObTextWhitespaceTokenizer bad_input;CHECK(bad_input.open(document,cs)==0);
  TrackingAllocator fail_arena;fail_arena.fail=true;
  ObBasicEnglishNormalizer failed;CHECK(failed.init(cs,bad_input)==0);
  configure(failed,fail_arena,0);
  ObDatum output;CHECK(failed.get_next(output,frequency)==OB_ALLOCATE_MEMORY_FAILED);
  std::cout<<"PASS caller-owned output, exact lowercase, multi-token/reuse/reset lifetime, allocation failure\n";
}

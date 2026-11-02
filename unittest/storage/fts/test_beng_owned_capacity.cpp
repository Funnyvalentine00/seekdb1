#include "share/text_analysis/ob_token_stream.h"
#include "lib/charset/ob_charset.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using namespace oceanbase::common;
using namespace oceanbase::share;
class CapacityAllocator final : public ObIAllocator {
public:
  int64_t requested=0,calls=0;
  std::vector<void *> blocks;
  ~CapacityAllocator() { for(auto p:blocks)std::free(p); }
  void *alloc(int64_t n) override { auto p=std::malloc(n);if(p){requested+=n;++calls;blocks.push_back(p);}return p; }
  void *alloc(int64_t n,const ObMemAttr &) override { return alloc(n); }
  void free(void *) override {}
};
#define CHECK(x) do { if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<'\n';return 1;} }while(0)
int main() {
  ObCharset::init_charset();
  for(auto col:{CS_TYPE_UTF8MB4_GENERAL_CI,CS_TYPE_UTF8MB4_BIN,CS_TYPE_UTF8MB4_UNICODE_CI}) {
    CapacityAllocator caller;std::string input;
    for(int i=0;i<4096;++i)input+="\xe2\x84\xaa";
    input+=" ";for(int i=0;i<4096;++i)input+="\xc4\xb0";
    input+=" HELLO world Résumé";
    std::vector<ObDatum> kept;int64_t bytes=0;
    {
      ObDatum doc;doc.set_string(input.data(),input.size());
      auto cs=ObCharset::get_charset(col);
      ObTextWhitespaceTokenizer tokenizer;CHECK(tokenizer.open(doc,cs)==0);
      ObBasicEnglishNormalizer normalizer;CHECK(normalizer.init(cs,tokenizer)==0);
      normalizer.set_output_allocator(caller);int ret=0;int64_t freq=0;
      while(ret==0) {
        ObDatum token;ret=normalizer.get_next(token,freq);if(ret!=0)break;
        CHECK(freq==1);bytes+=token.len_;kept.push_back(token);
        CHECK(caller.requested==bytes);
      }
      CHECK(ret==OB_ITER_END);normalizer.reuse();normalizer.reset();
    }
    CHECK(kept.size()==5 && caller.calls==5 && caller.requested==bytes);
    CHECK(std::string(kept[0].ptr_,kept[0].len_)==std::string(4096,'k'));
    CHECK(std::string(kept[1].ptr_,kept[1].len_)==std::string(4096,'i'));
    CHECK(std::string(kept[2].ptr_,kept[2].len_)=="hello");
    CHECK(std::string(kept[3].ptr_,kept[3].len_)=="world");
    CHECK(std::string(kept[4].ptr_,kept[4].len_)=="résumé");
  }
  std::cout<<"PASS exact retained bytes for ASCII/mixed/shrinking UTF8, three collations and post-reset lifetime\n";
}

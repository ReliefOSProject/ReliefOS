#define T(text) translation_macro_must_not_expand_here(text)
#include "storage.h"
#include <assert.h>
#include <stdio.h>
int main() {
    rx::Array<rx::Text> values;
    rx::Text text; assert(text.set("中文条目"));
    for (int i=0;i<8;++i) assert(values.push(text));
    assert(values.push(values[0]));
    assert(values[8] == "中文条目");
    for (int i=0;i<7;++i) assert(values.push(text));
    assert(values.insert(0,values[8]));
    assert(values[0] == "中文条目");
    rx::Array<rx::Text> other; assert(other.clone_from(values));
    values.clear(); assert(other[0] == "中文条目");
    rx::Array<rx::Text> moved(static_cast<rx::Array<rx::Text>&&>(other));
    assert(other.empty() && moved.size()==17);
    text.set(text.c_str()+3); assert(text == "文条目");
    puts("reliefexcel storage: passed");
}

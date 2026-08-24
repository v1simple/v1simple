#pragma once

#include <cstddef>

#include "product_event.h"

inline constexpr char kProductEventSchemaHeader[] =
    "# product_event_schema=1\n"
    "ms,source,event,id,sequence,item,count,payload\n";

size_t productEventRowCount(const ProductEvent& event);
size_t serializeProductEventRow(const ProductEvent& event, size_t item, char* output, size_t capacity);

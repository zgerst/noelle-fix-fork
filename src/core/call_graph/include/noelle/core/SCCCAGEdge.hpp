/*
 * Copyright 2023 - 2024  Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef NOELLE_SRC_CORE_CALL_GRAPH_SCCCAGEDGE_H_
#define NOELLE_SRC_CORE_CALL_GRAPH_SCCCAGEDGE_H_

#include "noelle/core/SCCCAGNode.hpp"

namespace arcana::noelle {

class SCCCAGEdge {
public:
  SCCCAGEdge(SCCCAGNode *from, SCCCAGNode *to);

  SCCCAGNode *getSrc(void) const;

  SCCCAGNode *getDst(void) const;

  void addSubEdge(CallGraphFunctionFunctionEdge *cgEdge);

  std::unordered_set<CallGraphFunctionFunctionEdge *> getSubEdges(void) const;

private:
  SCCCAGNode *f;
  SCCCAGNode *t;
  std::unordered_set<CallGraphFunctionFunctionEdge *> subEdges;
};

} // namespace arcana::noelle

#endif // NOELLE_SRC_CORE_CALL_GRAPH_SCCCAGEDGE_H_

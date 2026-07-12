export interface FlameNode {
  name: string
  value: number
  selfValue: number
  children: FlameNode[]
  depth: number
}

export interface StackSample {
  stack: string[]
  count: number
}

export interface WorkerMessage {
  type: 'build' | 'diff' | 'search' | 'merge'
  id: string
  payload: unknown
}

export interface BuildPayload {
  samples: StackSample[]
}

export interface DiffPayload {
  baseline: StackSample[]
  current: StackSample[]
}

export interface SearchPayload {
  root: FlameNode
  pattern: string
}

export interface MergePayload {
  roots: FlameNode[]
}

export interface WorkerResponse {
  type: 'result'
  id: string
  payload: unknown
}

function buildFlameTree(samples: StackSample[]): FlameNode {
  const root: FlameNode = { name: 'root', value: 0, selfValue: 0, children: [], depth: 0 }
  // 使用 Map 缓存子节点，避免每层 O(n) 线性查找
  const childMap = new Map<FlameNode, Map<string, FlameNode>>()
  childMap.set(root, new Map())

  for (const sample of samples) {
    let node = root
    root.value += sample.count

    for (let i = 0; i < sample.stack.length; i++) {
      const name = sample.stack[i]
      let children = childMap.get(node)
      if (!children) {
        children = new Map()
        childMap.set(node, children)
      }
      let child = children.get(name)
      if (!child) {
        child = { name, value: 0, selfValue: 0, children: [], depth: i + 1 }
        children.set(name, child)
        node.children.push(child)
        childMap.set(child, new Map())
      }
      child.value += sample.count
      node = child
    }
    node.selfValue += sample.count
  }

  return root
}

/**
 * 合并多棵火焰图树。用于把多个时间窗口/进程的 profile 聚合到一棵树。
 */
function mergeFlameTrees(roots: FlameNode[]): FlameNode {
  const merged: FlameNode = { name: 'root', value: 0, selfValue: 0, children: [], depth: 0 }
  const childMap = new Map<FlameNode, Map<string, FlameNode>>()
  childMap.set(merged, new Map())

  function addNode(target: FlameNode, source: FlameNode) {
    target.value += source.value
    target.selfValue += source.selfValue

    let targetChildren = childMap.get(target)
    if (!targetChildren) {
      targetChildren = new Map()
      childMap.set(target, targetChildren)
    }

    for (const sourceChild of source.children) {
      let targetChild = targetChildren.get(sourceChild.name)
      if (!targetChild) {
        targetChild = {
          name: sourceChild.name,
          value: 0,
          selfValue: 0,
          children: [],
          depth: target.depth + 1,
        }
        targetChildren.set(sourceChild.name, targetChild)
        target.children.push(targetChild)
        childMap.set(targetChild, new Map())
      }
      addNode(targetChild, sourceChild)
    }
  }

  for (const root of roots) {
    addNode(merged, root)
  }

  return merged
}

function buildDiffTree(baseline: StackSample[], current: StackSample[]): FlameNode & { diff?: number } {
  const baseTree = buildFlameTree(baseline)
  const currTree = buildFlameTree(current)

  const baseTotal = baseTree.value || 1
  const currTotal = currTree.value || 1

  function mergeDiff(base: FlameNode | null, curr: FlameNode | null, depth: number): FlameNode & { diff: number } {
    const name = curr?.name || base?.name || ''
    const basePct = base ? (base.value / baseTotal) * 100 : 0
    const currPct = curr ? (curr.value / currTotal) * 100 : 0

    const merged: FlameNode & { diff: number } = {
      name,
      value: curr?.value || base?.value || 0,
      selfValue: curr?.selfValue || 0,
      children: [],
      depth,
      diff: currPct - basePct,
    }

    const allNames = new Set<string>()
    if (base) base.children.forEach(c => allNames.add(c.name))
    if (curr) curr.children.forEach(c => allNames.add(c.name))

    for (const childName of allNames) {
      const baseChild = base?.children.find(c => c.name === childName) ?? null
      const currChild = curr?.children.find(c => c.name === childName) ?? null
      merged.children.push(mergeDiff(baseChild, currChild, depth + 1))
    }

    return merged
  }

  return mergeDiff(baseTree, currTree, 0)
}

function searchTree(root: FlameNode, pattern: string): { matches: string[]; totalPct: number } {
  const regex = new RegExp(pattern, 'i')
  const matches: string[] = []
  let matchedValue = 0

  function walk(node: FlameNode) {
    if (regex.test(node.name)) {
      matches.push(node.name)
      matchedValue += node.selfValue
    }
    for (const child of node.children) walk(child)
  }

  walk(root)
  const totalPct = root.value > 0 ? (matchedValue / root.value) * 100 : 0
  return { matches: [...new Set(matches)], totalPct }
}

self.onmessage = (e: MessageEvent<WorkerMessage>) => {
  const { type, id, payload } = e.data
  let result: unknown

  switch (type) {
    case 'build':
      result = buildFlameTree((payload as BuildPayload).samples)
      break
    case 'diff':
      result = buildDiffTree(
        (payload as DiffPayload).baseline,
        (payload as DiffPayload).current
      )
      break
    case 'search':
      result = searchTree(
        (payload as SearchPayload).root,
        (payload as SearchPayload).pattern
      )
      break
    case 'merge':
      result = mergeFlameTrees((payload as MergePayload).roots)
      break
  }

  const response: WorkerResponse = { type: 'result', id, payload: result }
  self.postMessage(response)
}

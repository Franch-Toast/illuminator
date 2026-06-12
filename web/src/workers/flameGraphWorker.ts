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
  type: 'build' | 'diff' | 'search'
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

export interface WorkerResponse {
  type: 'result'
  id: string
  payload: unknown
}

function buildFlameTree(samples: StackSample[]): FlameNode {
  const root: FlameNode = { name: 'root', value: 0, selfValue: 0, children: [], depth: 0 }

  for (const sample of samples) {
    let node = root
    root.value += sample.count

    for (let i = 0; i < sample.stack.length; i++) {
      const name = sample.stack[i]
      let child = node.children.find(c => c.name === name)
      if (!child) {
        child = { name, value: 0, selfValue: 0, children: [], depth: i + 1 }
        node.children.push(child)
      }
      child.value += sample.count
      node = child
    }
    node.selfValue += sample.count
  }

  return root
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
  }

  const response: WorkerResponse = { type: 'result', id, payload: result }
  self.postMessage(response)
}

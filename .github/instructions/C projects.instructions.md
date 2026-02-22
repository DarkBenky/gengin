---
applyTo: '**'
---

# Project Context and Coding Guidelines

## Code Style & Conventions
- **Naming**: Use `camelCase` for variables and functions, `PascalCase` for types and classes
- **Comments**: Only when necessary—explain **why**, not **what**. Avoid obvious or boilerplate comments
- **Formatting**: Consistent indentation, clear separation between logical blocks

## Architecture & Performance
- **Memory Layout**: Prefer **Structure-of-Arrays (SoA)** over Array-of-Structs for data-oriented design
- **Allocation Strategy**: Use arena allocators and memory pools. Minimize heap allocations
- **Data Locality**: Keep related data contiguous in memory for better cache performance
- **Reuse Over Allocation**: Reuse buffers, preallocate when sizes are known, avoid repeated allocations

## Design Principles
- **Simplicity First**: Avoid premature abstraction. Start simple, refactor when patterns emerge
- **Locality**: Keep related data and behavior together. Minimize cross-module dependencies
- **Explicitness**: Prefer explicit code over clever tricks. Clear > clever
- **Functions**: Small, focused functions with clear inputs/outputs. Avoid side effects when possible

## What to Generate
- **Minimal Boilerplate**: Skip unnecessary setup code, getters/setters, or wrapper functions
- **Focused Implementation**: Generate the core logic, not scaffolding
- **Idiomatic Code**: Follow language-specific best practices and standard library patterns
- **Error Handling**: Include explicit error handling, avoid silent failures
- **Documentation**: Do not generate extensive documentation—focus on code clarity instead so no extra docs are needed

## What to Avoid
- Over-engineering solutions for simple problems
- Excessive inheritance hierarchies or interface abstractions
- Magic numbers or undocumented constants
- Premature optimization without profiling
- Hidden allocations or performance traps
- At all costs, avoid using emojis other non-standard characters in code or comments and do not explain them
- Generating tests unless explicitly requested
- Generate Readme or other documentation files unless explicitly requested or are useful for your context
- Avoid creating redundant comments that do not add value

## Context Awareness
- Consider existing codebase patterns and match that style
- Suggest performance-critical improvements when relevant
- Highlight potential memory or performance issues
- Recommend simpler alternatives when over-complexity is detected

when using OpenCL, prefer pinned memory and direct mapping for better performance. Avoid pageable host memory which incurs extra copying overhead.
```c
// SLOW — pageable host memory, extra memcpy internally
float* buf = malloc(size);
clEnqueueReadBuffer(..., buf, ...);  // ~2-3x slower

// FAST — pinned memory, DMA direct transfer
cl_mem pinned = clCreateBuffer(ctx, CL_MEM_ALLOC_HOST_PTR, size, NULL, NULL);
float* buf = clEnqueueMapBuffer(..., pinned, ...);  // near theoretical bandwidth
```
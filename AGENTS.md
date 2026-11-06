# Project Agent Guidance

## 工程施工标准（必读）

本仓库所有 Agent 施工须先阅读并遵循 [AI Agent 工程施工规范 v1.0](AGENT_ENGINEERING_STANDARD.md)。该文件为用户指定的完整施工标准，适用于 Codex、Trae 等具备工程操作能力的 Agent。

- 每阶段记录环境、范围、验证证据、Checkpoint、已知问题及回滚方式。
- 保留实验基线、日志和失败结果；不得降低测试标准或把未执行测试记为通过。
- 优先复用已验证的工具链、依赖和缓存；操作前重新确认实际用户、路径和资源，不沿用失效的 bench 用户配置。
- 有损操作遵循该规范的数据分类、备份和授权边界；资源受限时调整方案，不反复执行相同失败操作。
- 结束时按规范第十节提供七项交付信息；存在阻塞项时明确标记未全面验收。

## C++ Type Naming

- New C++ classes, including interface classes, do not need the legacy `Ob` prefix.
- Keep the `I` prefix for interface classes. For example, use `ICacheMemoryGetter` instead of `ObICacheMemoryGetter`.
- Do not rename existing types only to remove the `Ob` prefix unless the task explicitly requires it.

## C++ Headers

- Use `#pragma once` in new C++ header files instead of macro-based `#ifndef`/`#define` include guards.

## Code Review

- For pull request or diff review tasks, read and follow `.agents/skills/code-review/SKILL.md`.

## Test Placement

- Prefer adding regression and correctness coverage to the repository's established test suites and test directories.
- Avoid adding test-only or benchmark-only files under `tools/` unless the task specifically requires a reusable developer tool.

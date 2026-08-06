# ─────────────────────────────────────────────────────────────────────────────
# bench —— 压测客户端
#
# ⚠️ 必须与服务端**同机运行**。
#    开发机在国内、服务器在新加坡，跨国 RTT 约 50~80ms；
#    压测客户端如果跑在本地，测出来的 P99 全是网络往返延迟，
#    不是服务端性能。跨国链路只用于功能联调，不用于出性能数字。
# ─────────────────────────────────────────────────────────────────────────────

find_package(Threads REQUIRED)

add_executable(bench_client bench_client.cpp)
target_link_libraries(bench_client PRIVATE Threads::Threads)
target_compile_options(bench_client PRIVATE -Wall -Wextra)

# 分阶段推送的端到端验证：消息时序、计算中断连、过载拒绝、畸形报文。
# 单测能证明零件对，证明不了装起来之后时序对 —— 那只能在真实 socket 上验。
add_executable(plan_client plan_client.cpp)
target_link_libraries(plan_client PRIVATE Threads::Threads)
target_compile_options(plan_client PRIVATE -Wall -Wextra)

# 数据访问协议的端到端验证与压测：完整 CRUD、批量 vs 逐条、并发、畸形请求
add_executable(data_client data_client.cpp)
target_link_libraries(data_client PRIVATE Threads::Threads)
target_compile_options(data_client PRIVATE -Wall -Wextra)

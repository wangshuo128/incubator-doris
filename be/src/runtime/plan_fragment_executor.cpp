// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
// This file is copied from
// https://github.com/cloudera/Impala/blob/v0.7refresh/be/src/runtime/plan-fragment-executor.cc
// and modified by Doris

#include "runtime/plan_fragment_executor.h"

#include <thrift/protocol/TDebugProtocol.h>

#include <unordered_map>

#include "exec/data_sink.h"
#include "exec/exchange_node.h"
#include "exec/exec_node.h"
#include "exec/scan_node.h"
#include "exprs/expr.h"
#include "runtime/data_stream_mgr.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/memory/mem_tracker.h"
#include "runtime/result_buffer_mgr.h"
#include "runtime/result_queue_mgr.h"
#include "runtime/row_batch.h"
#include "runtime/thread_context.h"
#include "util/container_util.hpp"
#include "util/defer_op.h"
#include "util/logging.h"
#include "util/mem_info.h"
#include "util/parse_util.h"
#include "util/pretty_printer.h"
#include "util/telemetry/telemetry.h"
#include "util/uid_util.h"
#include "vec/core/block.h"
#include "vec/exec/vexchange_node.h"
#include "vec/runtime/vdata_stream_mgr.h"

namespace doris {

PlanFragmentExecutor::PlanFragmentExecutor(ExecEnv* exec_env,
                                           const report_status_callback& report_status_cb)
        : _exec_env(exec_env),
          _plan(nullptr),
          _report_status_cb(report_status_cb),
          _report_thread_active(false),
          _done(false),
          _prepared(false),
          _closed(false),
          _is_report_success(true),
          _is_report_on_cancel(true),
          _collect_query_statistics_with_every_batch(false),
          _cancel_reason(PPlanFragmentCancelReason::INTERNAL_ERROR),
          _cancel_msg("") {}

PlanFragmentExecutor::~PlanFragmentExecutor() {
    close();
    // at this point, the report thread should have been stopped
    DCHECK(!_report_thread_active);
}

Status PlanFragmentExecutor::prepare(const TExecPlanFragmentParams& request,
                                     QueryFragmentsCtx* fragments_ctx) {
    OpentelemetryTracer tracer = telemetry::get_noop_tracer();
    if (opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext().IsValid()) {
        tracer = telemetry::get_tracer(print_id(_query_id));
    }
    START_AND_SCOPE_SPAN(tracer, span, "PlanFragmentExecutor::prepare");

    const TPlanFragmentExecParams& params = request.params;
    _query_id = params.query_id;

    TAG(LOG(INFO))
            .log("PlanFragmentExecutor::prepare")
            .query_id(_query_id)
            .instance_id(params.fragment_instance_id)
            .tag("backend_num", std::to_string(request.backend_num))
            .tag("pthread_id", std::to_string((uintptr_t)pthread_self()));
    // VLOG_CRITICAL << "request:\n" << apache::thrift::ThriftDebugString(request);

    const TQueryGlobals& query_globals =
            fragments_ctx == nullptr ? request.query_globals : fragments_ctx->query_globals;
    _runtime_state.reset(new RuntimeState(params, request.query_options, query_globals, _exec_env));
    _runtime_state->set_query_fragments_ctx(fragments_ctx);
    _runtime_state->set_tracer(std::move(tracer));

    RETURN_IF_ERROR(_runtime_state->init_mem_trackers(_query_id));
    SCOPED_ATTACH_TASK(_runtime_state.get());
    _runtime_state->set_be_number(request.backend_num);
    if (request.__isset.backend_id) {
        _runtime_state->set_backend_id(request.backend_id);
    }
    if (request.__isset.import_label) {
        _runtime_state->set_import_label(request.import_label);
    }
    if (request.__isset.db_name) {
        _runtime_state->set_db_name(request.db_name);
    }
    if (request.__isset.load_job_id) {
        _runtime_state->set_load_job_id(request.load_job_id);
    }
    if (request.__isset.load_error_hub_info) {
        _runtime_state->set_load_error_hub_info(request.load_error_hub_info);
    }

    if (request.query_options.__isset.is_report_success) {
        _is_report_success = request.query_options.is_report_success;
    }

    RETURN_IF_ERROR(_runtime_state->create_block_mgr());

    // set up desc tbl
    DescriptorTbl* desc_tbl = nullptr;
    if (fragments_ctx != nullptr) {
        desc_tbl = fragments_ctx->desc_tbl;
    } else {
        DCHECK(request.__isset.desc_tbl);
        RETURN_IF_ERROR(DescriptorTbl::create(obj_pool(), request.desc_tbl, &desc_tbl));
    }
    _runtime_state->set_desc_tbl(desc_tbl);

    // set up plan
    DCHECK(request.__isset.fragment);
    RETURN_IF_ERROR(ExecNode::create_tree(_runtime_state.get(), obj_pool(), request.fragment.plan,
                                          *desc_tbl, &_plan));
    _runtime_state->set_fragment_root_id(_plan->id());

    // set #senders of exchange nodes before calling Prepare()
    std::vector<ExecNode*> exch_nodes;
    _plan->collect_nodes(TPlanNodeType::EXCHANGE_NODE, &exch_nodes);
    for (ExecNode* exch_node : exch_nodes) {
        DCHECK_EQ(exch_node->type(), TPlanNodeType::EXCHANGE_NODE);
        int num_senders = find_with_default(params.per_exch_num_senders, exch_node->id(), 0);
        DCHECK_GT(num_senders, 0);
        if (_runtime_state->enable_vectorized_exec()) {
            static_cast<doris::vectorized::VExchangeNode*>(exch_node)->set_num_senders(num_senders);
        } else {
            static_cast<ExchangeNode*>(exch_node)->set_num_senders(num_senders);
        }
    }

    RETURN_IF_ERROR(_plan->prepare(_runtime_state.get()));
    // set scan ranges
    std::vector<ExecNode*> scan_nodes;
    std::vector<TScanRangeParams> no_scan_ranges;
    _plan->collect_scan_nodes(&scan_nodes);
    VLOG_CRITICAL << "scan_nodes.size()=" << scan_nodes.size();
    VLOG_CRITICAL << "params.per_node_scan_ranges.size()=" << params.per_node_scan_ranges.size();

    _plan->try_do_aggregate_serde_improve();

    for (int i = 0; i < scan_nodes.size(); ++i) {
        ScanNode* scan_node = static_cast<ScanNode*>(scan_nodes[i]);
        const std::vector<TScanRangeParams>& scan_ranges =
                find_with_default(params.per_node_scan_ranges, scan_node->id(), no_scan_ranges);
        scan_node->set_scan_ranges(scan_ranges);
        VLOG_CRITICAL << "scan_node_Id=" << scan_node->id() << " size=" << scan_ranges.size();
    }

    _runtime_state->set_per_fragment_instance_idx(params.sender_id);
    _runtime_state->set_num_per_fragment_instances(params.num_senders);

    // set up sink, if required
    if (request.fragment.__isset.output_sink) {
        RETURN_IF_ERROR(DataSink::create_data_sink(
                obj_pool(), request.fragment.output_sink, request.fragment.output_exprs, params,
                row_desc(), runtime_state()->enable_vectorized_exec(), &_sink, *desc_tbl));
        RETURN_IF_ERROR(_sink->prepare(runtime_state()));

        RuntimeProfile* sink_profile = _sink->profile();
        if (sink_profile != nullptr) {
            profile()->add_child(sink_profile, true, nullptr);
        }

        _collect_query_statistics_with_every_batch =
                params.__isset.send_query_statistics_with_every_batch
                        ? params.send_query_statistics_with_every_batch
                        : false;
    } else {
        // _sink is set to nullptr
        _sink.reset(nullptr);
    }

    // set up profile counters
    profile()->add_child(_plan->runtime_profile(), true, nullptr);
    _rows_produced_counter = ADD_COUNTER(profile(), "RowsProduced", TUnit::UNIT);
    _fragment_cpu_timer = ADD_TIMER(profile(), "FragmentCpuTime");

    _row_batch.reset(new RowBatch(_plan->row_desc(), _runtime_state->batch_size()));
    _block.reset(new doris::vectorized::Block());
    // _row_batch->tuple_data_pool()->set_limits(*_runtime_state->mem_trackers());
    VLOG_NOTICE << "plan_root=\n" << _plan->debug_string();
    _prepared = true;

    _query_statistics.reset(new QueryStatistics());
    if (_sink.get() != nullptr) {
        _sink->set_query_statistics(_query_statistics);
    }
    return Status::OK();
}

Status PlanFragmentExecutor::open() {
    int64_t mem_limit = _runtime_state->instance_mem_tracker()->limit();
    TAG(LOG(INFO))
            .log("PlanFragmentExecutor::open, using query memory limit: " +
                 PrettyPrinter::print(mem_limit, TUnit::BYTES))
            .query_id(_query_id)
            .instance_id(_runtime_state->fragment_instance_id())
            .tag("mem_limit", std::to_string(mem_limit));

    // we need to start the profile-reporting thread before calling Open(), since it
    // may block
    // TODO: if no report thread is started, make sure to send a final profile
    // at end, otherwise the coordinator hangs in case we finish w/ an error
    if (_is_report_success && _report_status_cb && config::status_report_interval > 0) {
        std::unique_lock<std::mutex> l(_report_thread_lock);
        _report_thread = std::thread(&PlanFragmentExecutor::report_profile, this);
        // make sure the thread started up, otherwise report_profile() might get into a race
        // with stop_report_thread()
        _report_thread_started_cv.wait(l);
    }
    Status status = Status::OK();
    if (_runtime_state->enable_vectorized_exec()) {
        status = open_vectorized_internal();
    } else {
        status = open_internal();
    }

    if (!status.ok() && !status.is_cancelled() && _runtime_state->log_has_space()) {
        // Log error message in addition to returning in Status. Queries that do not
        // fetch results (e.g. insert) may not receive the message directly and can
        // only retrieve the log.
        _runtime_state->log_error(status.get_error_msg());
    }
    if (status.is_cancelled()) {
        if (_cancel_reason == PPlanFragmentCancelReason::CALL_RPC_ERROR) {
            status = Status::RuntimeError(_cancel_msg);
        } else if (_cancel_reason == PPlanFragmentCancelReason::MEMORY_LIMIT_EXCEED) {
            status = Status::MemoryLimitExceeded(_cancel_msg);
        }
    }

    update_status(status);
    return status;
}

Status PlanFragmentExecutor::open_vectorized_internal() {
    {
        SCOPED_CPU_TIMER(_fragment_cpu_timer);
        SCOPED_TIMER(profile()->total_time_counter());
        RETURN_IF_ERROR(_plan->open(_runtime_state.get()));
    }
    if (_sink == nullptr) {
        return Status::OK();
    }
    {
        SCOPED_CPU_TIMER(_fragment_cpu_timer);
        RETURN_IF_ERROR(_sink->open(runtime_state()));
    }

    {
        auto sink_send_span_guard = Defer {[this]() { this->_sink->end_send_span(); }};
        while (true) {
            doris::vectorized::Block* block;

            {
                SCOPED_CPU_TIMER(_fragment_cpu_timer);
                RETURN_IF_ERROR(get_vectorized_internal(&block));
            }

            if (block == NULL) {
                break;
            }

            SCOPED_TIMER(profile()->total_time_counter());
            SCOPED_CPU_TIMER(_fragment_cpu_timer);
            // Collect this plan and sub plan statistics, and send to parent plan.
            if (_collect_query_statistics_with_every_batch) {
                _collect_query_statistics();
            }

            auto st = _sink->send(runtime_state(), block);
            if (st.is_end_of_file()) {
                break;
            }
            RETURN_IF_ERROR(st);
        }
    }

    {
        SCOPED_TIMER(profile()->total_time_counter());
        _collect_query_statistics();
        Status status;
        {
            std::lock_guard<std::mutex> l(_status_lock);
            status = _status;
        }
        status = _sink->close(runtime_state(), status);
        RETURN_IF_ERROR(status);
    }
    // Setting to NULL ensures that the d'tor won't double-close the sink.
    _sink.reset(nullptr);
    _done = true;

    stop_report_thread();
    send_report(true);

    return Status::OK();
}

Status PlanFragmentExecutor::get_vectorized_internal(::doris::vectorized::Block** block) {
    if (_done) {
        *block = nullptr;
        return Status::OK();
    }

    while (!_done) {
        _block->clear_column_data(_plan->row_desc().num_materialized_slots());
        SCOPED_TIMER(profile()->total_time_counter());
        RETURN_IF_ERROR_AND_CHECK_SPAN(_plan->get_next(_runtime_state.get(), _block.get(), &_done),
                                       _plan->get_next_span(), _done);

        if (_block->rows() > 0) {
            COUNTER_UPDATE(_rows_produced_counter, _block->rows());
            *block = _block.get();
            break;
        }

        *block = nullptr;
    }

    return Status::OK();
}

Status PlanFragmentExecutor::open_internal() {
    {
        SCOPED_CPU_TIMER(_fragment_cpu_timer);
        SCOPED_TIMER(profile()->total_time_counter());
        RETURN_IF_ERROR(_plan->open(_runtime_state.get()));
    }

    if (_sink.get() == nullptr) {
        return Status::OK();
    }
    {
        SCOPED_CPU_TIMER(_fragment_cpu_timer);
        RETURN_IF_ERROR(_sink->open(runtime_state()));
    }

    // If there is a sink, do all the work of driving it here, so that
    // when this returns the query has actually finished
    RowBatch* batch = nullptr;
    while (true) {
        {
            SCOPED_CPU_TIMER(_fragment_cpu_timer);
            RETURN_IF_ERROR(get_next_internal(&batch));
        }

        if (batch == nullptr) {
            break;
        }

        if (VLOG_ROW_IS_ON) {
            VLOG_ROW << "open_internal: #rows=" << batch->num_rows()
                     << " desc=" << row_desc().debug_string();

            for (int i = 0; i < batch->num_rows(); ++i) {
                TupleRow* row = batch->get_row(i);
                VLOG_ROW << row->to_string(row_desc());
            }
        }

        SCOPED_TIMER(profile()->total_time_counter());
        SCOPED_CPU_TIMER(_fragment_cpu_timer);
        // Collect this plan and sub plan statistics, and send to parent plan.
        if (_collect_query_statistics_with_every_batch) {
            _collect_query_statistics();
        }
        const Status& st = _sink->send(runtime_state(), batch);
        if (st.is_end_of_file()) {
            break;
        }
        RETURN_IF_ERROR(st);
    }

    // Close the sink *before* stopping the report thread. Close may
    // need to add some important information to the last report that
    // gets sent. (e.g. table sinks record the files they have written
    // to in this method)
    // The coordinator report channel waits until all backends are
    // either in error or have returned a status report with done =
    // true, so tearing down any data stream state (a separate
    // channel) in Close is safe.

    // TODO: If this returns an error, the d'tor will call Close again. We should
    // audit the sinks to check that this is ok, or change that behaviour.
    {
        SCOPED_TIMER(profile()->total_time_counter());
        _collect_query_statistics();
        Status status;
        {
            std::lock_guard<std::mutex> l(_status_lock);
            status = _status;
        }
        status = _sink->close(runtime_state(), status);
        RETURN_IF_ERROR(status);
    }

    // Setting to nullptr ensures that the d'tor won't double-close the sink.
    _sink.reset(nullptr);
    _done = true;

    stop_report_thread();
    send_report(true);

    return Status::OK();
}

void PlanFragmentExecutor::_collect_query_statistics() {
    _query_statistics->clear();
    _plan->collect_query_statistics(_query_statistics.get());
    _query_statistics->add_cpu_ms(_fragment_cpu_timer->value() / NANOS_PER_MILLIS);
    if (_runtime_state->backend_id() != -1) {
        _collect_node_statistics();
    }
}

void PlanFragmentExecutor::_collect_node_statistics() {
    DCHECK(_runtime_state->backend_id() != -1);
    NodeStatistics* node_statistics =
            _query_statistics->add_nodes_statistics(_runtime_state->backend_id());
    node_statistics->add_peak_memory(_runtime_state->instance_mem_tracker()->peak_consumption());
}

void PlanFragmentExecutor::report_profile() {
    SCOPED_ATTACH_TASK(_runtime_state.get());
    VLOG_FILE << "report_profile(): instance_id=" << _runtime_state->fragment_instance_id();
    DCHECK(_report_status_cb);

    _report_thread_active = true;

    std::unique_lock<std::mutex> l(_report_thread_lock);
    // tell Open() that we started
    _report_thread_started_cv.notify_one();

    // Jitter the reporting time of remote fragments by a random amount between
    // 0 and the report_interval.  This way, the coordinator doesn't get all the
    // updates at once so its better for contention as well as smoother progress
    // reporting.
    int report_fragment_offset = rand() % config::status_report_interval;
    // We don't want to wait longer than it takes to run the entire fragment.
    _stop_report_thread_cv.wait_for(l, std::chrono::seconds(report_fragment_offset));
    while (_report_thread_active) {
        if (config::status_report_interval > 0) {
            // wait_for can return because the timeout occurred or the condition variable
            // was signaled.  We can't rely on its return value to distinguish between the
            // two cases (e.g. there is a race here where the wait timed out but before grabbing
            // the lock, the condition variable was signaled).  Instead, we will use an external
            // flag, _report_thread_active, to coordinate this.
            _stop_report_thread_cv.wait_for(l,
                                            std::chrono::seconds(config::status_report_interval));
        } else {
            LOG(WARNING) << "config::status_report_interval is equal to or less than zero, exiting "
                            "reporting thread.";
            break;
        }

        if (VLOG_FILE_IS_ON) {
            VLOG_FILE << "Reporting " << (!_report_thread_active ? "final " : " ")
                      << "profile for instance " << _runtime_state->fragment_instance_id();
            std::stringstream ss;
            profile()->compute_time_in_profile();
            profile()->pretty_print(&ss);
            VLOG_FILE << ss.str();
        }

        if (!_report_thread_active) {
            break;
        }

        send_report(false);
    }

    VLOG_FILE << "exiting reporting thread: instance_id=" << _runtime_state->fragment_instance_id();
}

void PlanFragmentExecutor::send_report(bool done) {
    if (!_report_status_cb) {
        return;
    }

    Status status;
    {
        std::lock_guard<std::mutex> l(_status_lock);
        status = _status;
    }

    // If plan is done successfully, but _is_report_success is false,
    // no need to send report.
    if (!_is_report_success && done && status.ok()) {
        return;
    }

    // If both _is_report_success and _is_report_on_cancel are false,
    // which means no matter query is success or failed, no report is needed.
    // This may happen when the query limit reached and
    // a internal cancellation being processed
    if (!_is_report_success && !_is_report_on_cancel) {
        return;
    }

    // This will send a report even if we are cancelled.  If the query completed correctly
    // but fragments still need to be cancelled (e.g. limit reached), the coordinator will
    // be waiting for a final report and profile.
    if (_is_report_success) {
        _report_status_cb(status, profile(), done || !status.ok());
    } else {
        _report_status_cb(status, nullptr, done || !status.ok());
    }
}

void PlanFragmentExecutor::stop_report_thread() {
    if (!_report_thread_active) {
        return;
    }

    _report_thread_active = false;

    _stop_report_thread_cv.notify_one();
    _report_thread.join();
}

Status PlanFragmentExecutor::get_next(RowBatch** batch) {
    VLOG_FILE << "GetNext(): instance_id=" << _runtime_state->fragment_instance_id();
    Status status = get_next_internal(batch);
    update_status(status);

    if (_done) {
        TAG(LOG(INFO))
                .log("PlanFragmentExecutor::get_next finished")
                .query_id(_query_id)
                .instance_id(_runtime_state->fragment_instance_id());
        // Query is done, return the thread token
        stop_report_thread();
        send_report(true);
    }

    return status;
}

Status PlanFragmentExecutor::get_next_internal(RowBatch** batch) {
    if (_done) {
        *batch = nullptr;
        return Status::OK();
    }

    while (!_done) {
        _row_batch->reset();
        SCOPED_TIMER(profile()->total_time_counter());
        RETURN_IF_ERROR(_plan->get_next(_runtime_state.get(), _row_batch.get(), &_done));

        if (_row_batch->num_rows() > 0) {
            COUNTER_UPDATE(_rows_produced_counter, _row_batch->num_rows());
            *batch = _row_batch.get();
            break;
        }

        *batch = nullptr;
    }

    return Status::OK();
}

void PlanFragmentExecutor::update_status(const Status& new_status) {
    if (new_status.ok()) {
        return;
    }

    {
        std::lock_guard<std::mutex> l(_status_lock);
        // if current `_status` is ok, set it to `new_status` to record the error.
        if (_status.ok()) {
            if (new_status.is_mem_limit_exceeded()) {
                _runtime_state->set_mem_limit_exceeded(new_status.get_error_msg());
            }
            _status = new_status;
            if (_runtime_state->query_type() == TQueryType::EXTERNAL) {
                TUniqueId fragment_instance_id = _runtime_state->fragment_instance_id();
                _exec_env->result_queue_mgr()->update_queue_status(fragment_instance_id,
                                                                   new_status);
            }
        }
    }

    stop_report_thread();
    send_report(true);
}

void PlanFragmentExecutor::cancel(const PPlanFragmentCancelReason& reason, const std::string& msg) {
    TAG(LOG(INFO))
            .log("PlanFragmentExecutor::cancel")
            .query_id(_query_id)
            .instance_id(_runtime_state->fragment_instance_id());
    DCHECK(_prepared);
    _cancel_reason = reason;
    _cancel_msg = msg;
    _runtime_state->set_is_cancelled(true);

    // must close stream_mgr to avoid dead lock in Exchange Node
    auto env = _runtime_state->exec_env();
    auto id = _runtime_state->fragment_instance_id();
    if (_runtime_state->enable_vectorized_exec()) {
        env->vstream_mgr()->cancel(id);
    } else {
        env->stream_mgr()->cancel(id);
        env->result_mgr()->cancel(id);
    }
}

void PlanFragmentExecutor::set_abort() {
    update_status(Status::Aborted("Execution aborted before start"));
}

const RowDescriptor& PlanFragmentExecutor::row_desc() {
    return _plan->row_desc();
}

RuntimeProfile* PlanFragmentExecutor::profile() {
    return _runtime_state->runtime_profile();
}

void PlanFragmentExecutor::close() {
    if (_closed) {
        return;
    }

    _row_batch.reset(nullptr);

    // Prepare may not have been called, which sets _runtime_state
    if (_runtime_state.get() != nullptr) {
        // _runtime_state init failed
        if (_plan != nullptr) {
            _plan->close(_runtime_state.get());
        }

        if (_sink.get() != nullptr) {
            if (_prepared) {
                Status status;
                {
                    std::lock_guard<std::mutex> l(_status_lock);
                    status = _status;
                }
                _sink->close(runtime_state(), status);
            } else {
                _sink->close(runtime_state(), Status::InternalError("prepare failed"));
            }
        }

        if (_is_report_success) {
            std::stringstream ss;
            // Compute the _local_time_percent before pretty_print the runtime_profile
            // Before add this operation, the print out like that:
            // UNION_NODE (id=0):(Active: 56.720us, non-child: 00.00%)
            // After add the operation, the print out like that:
            // UNION_NODE (id=0):(Active: 56.720us, non-child: 82.53%)
            // We can easily know the exec node execute time without child time consumed.
            _runtime_state->runtime_profile()->compute_time_in_profile();
            _runtime_state->runtime_profile()->pretty_print(&ss);
            LOG(INFO) << ss.str();
        }
        LOG(INFO) << "Close() fragment_instance_id="
                  << print_id(_runtime_state->fragment_instance_id());
    }

    _closed = true;
}

} // namespace doris

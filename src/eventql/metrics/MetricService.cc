
/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "eventql/metrics/MetricService.h"

using namespace stx;

namespace zbase {

MetricService::MetricService(
    ConfigDirectory* cdir,
    AnalyticsAuth* auth,
    zbase::TSDBService* tsdb,
    zbase::PartitionMap* pmap,
    zbase::ReplicationScheme* repl,
    const String& cachedir) :
    cdir_(cdir),
    auth_(auth),
    tsdb_(tsdb),
    pmap_(pmap),
    repl_(repl),
    cachedir_(cachedir) {}

void MetricService::executeQuery(
    const AnalyticsSession& session,
    RefPtr<MetricQuery> query) {
  iputs("execute query..", 1);
}

} // namespace zbase
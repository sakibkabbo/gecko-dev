# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
The objective of optimization is to remove as many tasks from the graph as
possible, as efficiently as possible, thereby delivering useful results as
quickly as possible.  For example, ideally if only a test script is modified in
a push, then the resulting graph contains only the corresponding test suite
task.

See ``taskcluster/docs/optimization.rst`` for more information.
"""

from __future__ import absolute_import, print_function, unicode_literals

import logging
import os
from collections import defaultdict

from .graph import Graph
from . import files_changed
from .taskgraph import TaskGraph
from .util.seta import is_low_value_task
from .util.perfile import perfile_number_of_chunks
from .util.taskcluster import find_task_id
from .util.parameterization import resolve_task_references
from mozbuild.util import memoize
from slugid import nice as slugid
from mozbuild.base import MozbuildObject

logger = logging.getLogger(__name__)

TOPSRCDIR = os.path.abspath(os.path.join(__file__, '../../../'))


def optimize_task_graph(target_task_graph, params, do_not_optimize,
                        existing_tasks=None, strategies=None):
    """
    Perform task optimization, returning a taskgraph and a map from label to
    assigned taskId, including replacement tasks.
    """
    label_to_taskid = {}
    if not existing_tasks:
        existing_tasks = {}

    # instantiate the strategies for this optimization process
    if not strategies:
        strategies = _make_default_strategies()

    optimizations = _get_optimizations(target_task_graph, strategies)

    removed_tasks = remove_tasks(
        target_task_graph=target_task_graph,
        optimizations=optimizations,
        params=params,
        do_not_optimize=do_not_optimize)

    replaced_tasks = replace_tasks(
        target_task_graph=target_task_graph,
        optimizations=optimizations,
        params=params,
        do_not_optimize=do_not_optimize,
        label_to_taskid=label_to_taskid,
        existing_tasks=existing_tasks,
        removed_tasks=removed_tasks)

    return get_subgraph(
            target_task_graph, removed_tasks, replaced_tasks,
            label_to_taskid), label_to_taskid


def _make_default_strategies():
    return {
        'never': OptimizationStrategy(),  # "never" is the default behavior
        'index-search': IndexSearch(),
        'seta': SETA(),
        'skip-unless-changed': SkipUnlessChanged(),
        'skip-unless-schedules': SkipUnlessSchedules(),
        'skip-unless-schedules-or-seta': Either(SkipUnlessSchedules(), SETA()),
    }


def _get_optimizations(target_task_graph, strategies):
    def optimizations(label):
        task = target_task_graph.tasks[label]
        if task.optimization:
            opt_by, arg = task.optimization.items()[0]
            return (opt_by, strategies[opt_by], arg)
        else:
            return ('never', strategies['never'], None)
    return optimizations


def _log_optimization(verb, opt_counts):
    if opt_counts:
        logger.info(
            '{} '.format(verb.title()) +
            ', '.join(
                '{} tasks by {}'.format(c, b)
                for b, c in sorted(opt_counts.iteritems())) +
            ' during optimization.')
    else:
        logger.info('No tasks {} during optimization'.format(verb))


def remove_tasks(target_task_graph, params, optimizations, do_not_optimize):
    """
    Implement the "Removing Tasks" phase, returning a set of task labels of all removed tasks.
    """
    opt_counts = defaultdict(int)
    removed = set()
    reverse_links_dict = target_task_graph.graph.reverse_links_dict()

    for label in target_task_graph.graph.visit_preorder():
        # if we're not allowed to optimize, that's easy..
        if label in do_not_optimize:
            continue

        # if there are remaining tasks depending on this one, do not remove..
        if any(l not in removed for l in reverse_links_dict[label]):
            continue

        # call the optimization strategy
        task = target_task_graph.tasks[label]
        opt_by, opt, arg = optimizations(label)
        if opt.should_remove_task(task, params, arg):
            removed.add(label)
            opt_counts[opt_by] += 1
            continue

    _log_optimization('removed', opt_counts)
    return removed


def replace_tasks(target_task_graph, params, optimizations, do_not_optimize,
                  label_to_taskid, removed_tasks, existing_tasks):
    """
    Implement the "Replacing Tasks" phase, returning a set of task labels of
    all replaced tasks. The replacement taskIds are added to label_to_taskid as
    a side-effect.
    """
    opt_counts = defaultdict(int)
    replaced = set()
    links_dict = target_task_graph.graph.links_dict()

    for label in target_task_graph.graph.visit_postorder():
        # if we're not allowed to optimize, that's easy..
        if label in do_not_optimize:
            continue

        # if this task depends on un-replaced, un-removed tasks, do not replace
        if any(l not in replaced and l not in removed_tasks for l in links_dict[label]):
            continue

        # if the task already exists, that's an easy replacement
        repl = existing_tasks.get(label)
        if repl:
            label_to_taskid[label] = repl
            replaced.add(label)
            opt_counts['existing_tasks'] += 1
            continue

        # call the optimization strategy
        task = target_task_graph.tasks[label]
        opt_by, opt, arg = optimizations(label)
        repl = opt.should_replace_task(task, params, arg)
        if repl:
            if repl is True:
                # True means remove this task; get_subgraph will catch any
                # problems with removed tasks being depended on
                removed_tasks.add(label)
            else:
                label_to_taskid[label] = repl
                replaced.add(label)
            opt_counts[opt_by] += 1
            continue

    _log_optimization('replaced', opt_counts)
    return replaced


def get_subgraph(target_task_graph, removed_tasks, replaced_tasks, label_to_taskid):
    """
    Return the subgraph of target_task_graph consisting only of
    non-optimized tasks and edges between them.

    To avoid losing track of taskIds for tasks optimized away, this method
    simultaneously substitutes real taskIds for task labels in the graph, and
    populates each task definition's `dependencies` key with the appropriate
    taskIds.  Task references are resolved in the process.
    """

    # check for any dependency edges from included to removed tasks
    bad_edges = [(l, r, n) for l, r, n in target_task_graph.graph.edges
                 if l not in removed_tasks and r in removed_tasks]
    if bad_edges:
        probs = ', '.join('{} depends on {} as {} but it has been removed'.format(l, r, n)
                          for l, r, n in bad_edges)
        raise Exception("Optimization error: " + probs)

    # fill in label_to_taskid for anything not removed or replaced
    assert replaced_tasks <= set(label_to_taskid)
    for label in sorted(target_task_graph.graph.nodes - removed_tasks - set(label_to_taskid)):
        label_to_taskid[label] = slugid()

    # resolve labels to taskIds and populate task['dependencies']
    tasks_by_taskid = {}
    named_links_dict = target_task_graph.graph.named_links_dict()
    omit = removed_tasks | replaced_tasks
    for label, task in target_task_graph.tasks.iteritems():
        if label in omit:
            continue
        task.task_id = label_to_taskid[label]
        named_task_dependencies = {
            name: label_to_taskid[label]
            for name, label in named_links_dict.get(label, {}).iteritems()}
        task.task = resolve_task_references(task.label, task.task, named_task_dependencies)
        deps = task.task.setdefault('dependencies', [])
        deps.extend(sorted(named_task_dependencies.itervalues()))
        tasks_by_taskid[task.task_id] = task

    # resolve edges to taskIds
    edges_by_taskid = (
        (label_to_taskid.get(left), label_to_taskid.get(right), name)
        for (left, right, name) in target_task_graph.graph.edges
    )
    # ..and drop edges that are no longer entirely in the task graph
    #   (note that this omits edges to replaced tasks, but they are still in task.dependnecies)
    edges_by_taskid = set(
        (left, right, name)
        for (left, right, name) in edges_by_taskid
        if left in tasks_by_taskid and right in tasks_by_taskid
    )

    return TaskGraph(
        tasks_by_taskid,
        Graph(set(tasks_by_taskid), edges_by_taskid))


class OptimizationStrategy(object):
    def should_remove_task(self, task, params, arg):
        """Determine whether to optimize this task by removing it.  Returns
        True to remove."""
        return False

    def should_replace_task(self, task, params, arg):
        """Determine whether to optimize this task by replacing it.  Returns a
        taskId to replace this task, True to replace with nothing, or False to
        keep the task."""
        return False


class Either(OptimizationStrategy):
    """Given one or more optimization strategies, remove a task if any of them
    says to, and replace with a task if any finds a replacement (preferring the
    earliest).  By default, each substrategy gets the same arg, but split_args
    can return a list of args for each strategy, if desired."""
    def __init__(self, *substrategies, **kwargs):
        self.substrategies = substrategies
        self.split_args = kwargs.pop('split_args', None)
        if not self.split_args:
            self.split_args = lambda arg: [arg] * len(substrategies)
        if kwargs:
            raise TypeError("unexpected keyword args")

    def _for_substrategies(self, arg, fn):
        for sub, arg in zip(self.substrategies, self.split_args(arg)):
            rv = fn(sub, arg)
            if rv:
                return rv
        return False

    def should_remove_task(self, task, params, arg):
        return self._for_substrategies(
            arg,
            lambda sub, arg: sub.should_remove_task(task, params, arg))

    def should_replace_task(self, task, params, arg):
        return self._for_substrategies(
            arg,
            lambda sub, arg: sub.should_replace_task(task, params, arg))


class IndexSearch(OptimizationStrategy):

    # A task with no dependencies remaining after optimization will be replaced
    # if artifacts exist for the corresponding index_paths.
    # Otherwise, we're in one of the following cases:
    # - the task has un-optimized dependencies
    # - the artifacts have expired
    # - some changes altered the index_paths and new artifacts need to be
    # created.
    # In every of those cases, we need to run the task to create or refresh
    # artifacts.

    def should_replace_task(self, task, params, index_paths):
        "Look for a task with one of the given index paths"
        for index_path in index_paths:
            try:
                task_id = find_task_id(
                    index_path,
                    use_proxy=bool(os.environ.get('TASK_ID')))
                return task_id
            except KeyError:
                # 404 will end up here and go on to the next index path
                pass

        return False


class SETA(OptimizationStrategy):
    def should_remove_task(self, task, params, _):
        label = task.label

        # we would like to return 'False, None' while it's high_value_task
        # and we wouldn't optimize it. Otherwise, it will return 'True, None'
        if is_low_value_task(label,
                             params.get('project'),
                             params.get('pushlog_id'),
                             params.get('pushdate')):
            # Always optimize away low-value tasks
            return True
        else:
            return False


class SkipUnlessChanged(OptimizationStrategy):
    def should_remove_task(self, task, params, file_patterns):
        # pushlog_id == -1 - this is the case when run from a cron.yml job
        if params.get('pushlog_id') == -1:
            return False

        changed = files_changed.check(params, file_patterns)
        if not changed:
            logger.debug('no files found matching a pattern in `skip-unless-changed` for ' +
                         task.label)
            return True
        return False


class SkipUnlessSchedules(OptimizationStrategy):

    @memoize
    def scheduled_by_push(self, repository, revision):
        changed_files = files_changed.get_changed_files(repository, revision)

        mbo = MozbuildObject.from_environment()
        # the decision task has a sparse checkout, so, mozbuild_reader will use
        # a MercurialRevisionFinder with revision '.', which should be the same
        # as `revision`; in other circumstances, it will use a default reader
        rdr = mbo.mozbuild_reader(config_mode='empty')

        components = set()
        for p, m in rdr.files_info(changed_files).items():
            components |= set(m['SCHEDULES'].components)

        return components

    def should_remove_task(self, task, params, conditions):
        if params.get('pushlog_id') == -1:
            return False

        scheduled = self.scheduled_by_push(params['head_repository'], params['head_rev'])
        conditions = set(conditions)
        # if *any* of the condition components are scheduled, do not optimize
        if conditions & scheduled:
            return False

        return True


class TestVerify(OptimizationStrategy):
    def should_remove_task(self, task, params, _):
        # we would like to return 'False, None' while it's high_value_task
        # and we wouldn't optimize it. Otherwise, it will return 'True, None'
        env = params.get('try_task_config', {}) or {}
        env = env.get('templates', {}).get('env', {})
        if perfile_number_of_chunks(params.is_try(),
                                    env.get('MOZHARNESS_TEST_PATHS', ''),
                                    params.get('head_repository', ''),
                                    params.get('head_rev', ''),
                                    task):
            return False
        return True

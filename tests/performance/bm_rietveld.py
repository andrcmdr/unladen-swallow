#! /usr/bin/python2.5

"""Wrapper script for testing the performance of the Rietveld templates.

This is intended to support Unladen Swallow's perf.py

This will have Django render templates from Rietveld with canned data as many
times as you specify (via the -n flag). The raw times to generate the template
will be dumped to stdout. This is more convenient for Unladen Swallow's uses:
it allows us to keep all our stats in perf.py.

The data used for this benchmark was generated by running a simple development
copy of Rietveld on the App Engine SDK, and invoking upload.py on the Rietveld
source tree several times with trivial changes.  The main template rendering
routine was modified to pickle the relevant parts of the context dictionary and
dump it to the log.  Certain parameters such as the 'request' and 'form' keys
had to be deleted, since the would not pickle.  However, they are not used in
the template, so this should have no effect.

The following code block was inserted into the respond function in views.py
before the call to render_to_response:

    ...
    try:
        # START INSERTED CODE
        import cPickle
        interesting_params = params.copy()
        interesting_params.pop("request", None)
        interesting_params.pop("form", None)
        logging.info(cPickle.dumps((template, interesting_params)))
        # END INSERTED CODE
        return render_to_response(template, params)
    ...
"""

from __future__ import with_statement

__author__ = "rnk@google.com (Reid Kleckner)"

# Python imports
import cPickle
import optparse
import os
import time

# Django imports
from django.template import Context, loader, libraries, add_to_builtins

# Appengine imports
from google.appengine.tools import dev_appserver


def rel_path(path):
    return os.path.join(os.path.dirname(__file__), path)


def setup():
    # Appengine needs this setup.
    os.environ["SERVER_SOFTWARE"] = "Dev"
    os.environ["AUTH_DOMAIN"] = "gmail.com"
    os.environ["USER_EMAIL"] = "test@example.com"
    datastore_path = rel_path("rietveld_datastore")
    history_path = rel_path("rietveld_datastore_history")
    options = {"datastore_path": datastore_path,
               "history_path": history_path,
               "clear_datastore": False,
               "login_url": "/_ah/login",
               }
    dev_appserver.SetupStubs("codereview", **options)
    # Rietveld needs its template libraries loaded like this.
    library_name = "codereview.library"
    if not libraries.get(library_name, None):
        add_to_builtins(library_name)


def get_benchmark_data():
    # Load data.
    data_file = rel_path("rietveld_data.pickle")
    templ_name, canned_data = cPickle.load(open(data_file))
    context = Context(canned_data)

    # Load template.
    tmpl = loader.get_template(templ_name)
    return tmpl, context


def test_rietveld(tmpl, context, count):
    # Warm up Django.
    tmpl.render(context)
    tmpl.render(context)

    times = []
    for _ in xrange(count):
        t0 = time.time()
        # 30 calls to render, so that we don't measure loop overhead.
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        tmpl.render(context)
        t1 = time.time()
        times.append(t1 - t0)
    return times


if __name__ == "__main__":
    setup()
    parser = optparse.OptionParser(
        usage="%prog [options]",
        description=("Test the performance of Django templates using "
                     "Rietveld's front page template."))
    parser.add_option("-n", action="store", type="int", default=100,
                      dest="num_runs", help="Number of times to run the test.")
    parser.add_option("--profile", action="store_true",
                      help="Run the benchmark through cProfile.")
    parser.add_option("--profile_sort", action="store", type="str",
                      default="time", help="Column to sort cProfile output by.")
    options, args = parser.parse_args()

    tmpl, context = get_benchmark_data()
    if options.profile:
        import cProfile
        prof = cProfile.Profile()
        prof.runcall(test_rietveld, tmpl, context, options.num_runs)
        prof.print_stats(sort=options.profile_sort)
    else:
        for t in test_rietveld(tmpl, context, options.num_runs):
            print t

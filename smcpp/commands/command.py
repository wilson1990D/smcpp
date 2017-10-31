# Base class; subclasses will automatically show up as subcommands
import numpy as np
import argparse
import os
import os.path
import sys

from .. import logging, _smcpp
import smcpp.defaults

logger = logging.getLogger(__name__)

def check_positive(value):
    ivalue = int(value)
    if ivalue <= 0:
         raise argparse.ArgumentTypeError("%s is an invalid positive int value" % value)
    return ivalue

class ConsoleCommand:
    def __init__(self, parser):
        pass

class Command:
    def __init__(self, parser):
        '''Configure parser and parse args.'''
        parser.add_argument('-v', '--verbose', action='count', default=0,
                help="increase debugging output, specify multiply times for more")
        parser.add_argument('--seed', type=int, default=0, help=argparse.SUPPRESS)
        parser.add_argument('--cores', type=int, default=None, 
                help="Number of worker processes / threads "
                     "to use in parallel calculations")

    def main(self, args):
        np.random.seed(args.seed)
        logging.setup_logging(args.verbose)
        smcpp.defaults.cores = args.cores

class EstimationCommand(Command):
    def __init__(self, parser):
        super().__init__(parser)
        add_common_estimation_args(parser)

    def main(self, args):
        if not os.path.isdir(args.outdir):
            os.makedirs(args.outdir)
        # Initialize the logger
        # Do this before calling super().main() so that
        # any debugging output generated there gets logged
        logging.add_debug_log(os.path.join(args.outdir, ".debug.txt"))
        super().main(args)
        logger.debug(sys.argv)
        logger.debug(args)

def add_common_estimation_args(parser):
    parser.add_argument("-o", "--outdir", help="output directory", default=".")
    data = parser.add_argument_group('data parameters')
    data.add_argument('--length-cutoff', help=argparse.SUPPRESS, type=int, default=None)
    data.add_argument('--nonseg-cutoff', '-c',
                      help="recode nonsegregating spans > cutoff as missing. "
                      "default: do not recode.",
                      type=int)
    data.add_argument('--thinning', help="only emit full SFS every <k>th site. (k > 0)",
                      default=None, type=check_positive, metavar="k")
    data.add_argument('--w', default=100, help=argparse.SUPPRESS, type=int)
    optimizer = parser.add_argument_group("Optimization parameters")
    optimizer.add_argument("--no-initialize", action="store_true", default=False, help=argparse.SUPPRESS)
    optimizer.add_argument('--em-iterations', type=int,
                           help="number of EM steps to perform", default=20)
    optimizer.add_argument('--algorithm',
                           choices=["BFGS", "L-BFGS-B", "TNC", "AdaMax", "Adam"],
                           default="L-BFGS-B", help=argparse.SUPPRESS)
    optimizer.add_argument('--no-multi', default=False, action="store_true",
                           help="do not update multiple blocks of coordinates at once")
    optimizer.add_argument("--ftol", type=float,
                           default=smcpp.defaults.ftol,
                           help="stopping criterion for relative improvement in loglik "
                           "in EM algorithm. algorithm will terminate when "
                           "|loglik' - loglik| / loglik < ftol")
    optimizer.add_argument('--xtol', type=float,
                           default=smcpp.defaults.xtol,
                           help=r"x tolerance for optimizer. "
                           "optimizer will stop when |x' - x|_\infty < xtol")
    optimizer.add_argument('--regularization-penalty', '-rp',
                           type=float, help="regularization penalty",
                           default=smcpp.defaults.regularization_penalty)
    optimizer.add_argument('--lambda', dest="lambda_", type=float, help=argparse.SUPPRESS)
    optimizer.add_argument('--regularization-degree', '-rd',
                           type=int, help="regularization degree",
                           default=smcpp.defaults.regularization_degree)
    add_hmm_args(parser)

def add_hmm_args(parser):
    polarization = parser.add_mutually_exclusive_group(required=False)
    polarization.add_argument("--unfold", action="store_true", default=False,
                              help="use unfolded SFS (alias for -p 0.0)")
    polarization.add_argument('--polarization-error', '-p',
                              metavar='p', type=float, default=0.5,
                              help="uncertainty parameter for polarized SFS: observation (a,b) "
                              "has probability [(1-p)*CSFS_{a,b} + p*CSFS_{2-a,n-2-b}]. "
                              "default: 0.5")

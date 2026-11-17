"""Check thread selection and weighted self/inclusive GL attribution."""
import contextlib
import io
import sys
from types import SimpleNamespace as N
import gl_share as G

def main():
    events=[(10,'game',20,'libsims2.so','draw',[]),
            (10,'game',30,'libsims2.so','guest',[('libGLES_mali.so','driver')]),
            (10,'game',50,'libsims2.so','guest',[]),
            (11,'game',20,'libother.so','draw',[]),
            (20,'s2-render',200,'libGLES_mali.so','driver',[])]
    class Reader:
        def SetRecordFile(self,p):self.index=-1
        def GetNextSample(self):
            self.index+=1
            if self.index>=len(events):return None
            t,n,p,*_=events[self.index];return N(tid=t,thread_comm=n,period=p)
        def GetSymbolOfCurrentSample(self):
            e=events[self.index];return N(dso_name=e[3],symbol_name=e[4])
        def GetCallChainOfCurrentSample(self):
            e=events[self.index];return N(nr=len(e[5]),entries=[N(symbol=N(dso_name=d,symbol_name=s)) for d,s in e[5]])
    sys.modules['simpleperf_report_lib']=N(ReportLib=Reader)
    G.gl_symbols=lambda objects:{'draw':'d3d9'}
    def run(thread=None):
        sys.argv=['gl_share.py','unused-profile','--objects','unused-objects']+(['--thread',thread] if thread else [])
        stdout,stderr=io.StringIO(),io.StringIO()
        with contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
            try:G.main()
            except SystemExit as e:return e.code,stdout.getvalue()
        return 0,stdout.getvalue()
    code,text=run('10');assert code==0 and 'self GL side: 20.0%, inclusive: 50.0%' in text
    code,text=run();assert code==0 and 'thread 20 s2-render' in text
    code,text=run('11');assert code==0 and 'self GL side: 0.0%, inclusive: 0.0%' in text
    assert run('game')[0]==2 and run('999')[0]==2
    events.clear();assert run()[0]==2
    print('PASS GL shares: weighted self/inclusive, correct DSO, explicit owner vs busiest render, ambiguous/missing/empty rejection')

if __name__=='__main__':main()

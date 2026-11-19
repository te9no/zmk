"""Fault injection on production functions; run with python3 test_host.py."""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[2] / "src"
transport = (SRC / "split/peripheral.c").read_text()
pointing = (SRC / "pointing/input_split.c").read_text()
source = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define LOG_WRN(...)
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define INPUT_EV_KEY 1
#define CONFIG_ZMK_INPUT_SPLIT_MAX_TRACKED_KEYS 5
#define CONFIG_INPUT_MODE_THREAD 1
struct k_work {int x;};
#define K_WORK_DELAYABLE_DEFINE(name,fn) int name
static int scheduled;
#define k_work_schedule(w,d) ((void)(w), scheduled=1)
struct k_spinlock {int held;};
typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock *l){assert(!l->held);return ++l->held;}
static void k_spin_unlock(struct k_spinlock *l,int k){assert(k==1);l->held=0;}
typedef int atomic_t;
static int atomic_cas(int *p,int a,int b){if (*p!=a)return 0;*p=b;return 1;}
static void atomic_clear(int *p){*p=0;}
struct api {int (*set_enabled)(bool);};
struct zmk_split_transport_peripheral {struct api *api;};
static const struct zmk_split_transport_peripheral *active_peripheral_transport;
static int error,calls;
static int enable(bool en){calls++;return error;}
static struct {int reg;void *dev;} proxy_inputs[]={{1,NULL},{2,NULL}};
static int full,n,last;
static int input_report(void *dev,int t,int code,int v,bool sync,int wait){
    if(full)return -ENOMSG;
    n++;last=v;return 0;
}
"""
source += transport[transport.index("int zmk_split_peripheral_select_transport("):
                    transport.index("static int transport_status_changed_cb")]
source += pointing[pointing.index("struct tracked_key"):pointing.index("#define ZIS_INST")]
source += r"""
int main(void){
    struct api api={enable};
    struct zmk_split_transport_peripheral t={&api};
    error=-EAGAIN;
    assert(zmk_split_peripheral_select_transport(&t)==error && !active_peripheral_transport);
    error=0;
    assert(!zmk_split_peripheral_select_transport(&t) && calls==2);
    assert(!zmk_split_peripheral_select_transport(&t) && calls==2);
    error=-EBUSY;
    assert(zmk_split_peripheral_select_transport(NULL)==error && active_peripheral_transport==&t);
    error=0;
    assert(!zmk_split_peripheral_select_transport(NULL) && !active_peripheral_transport);
    assert(!zmk_input_split_report_peripheral_event(1,1,272,1,true));
    full=1;
    assert(zmk_input_split_report_peripheral_event(1,1,272,0,true)==-ENOMSG && scheduled);
    assert(!zmk_input_split_peripheral_disconnected(1));
    full=0;
    retry_releases(NULL);retry_releases(NULL);
    assert(n==2 && !last);
    assert(!zmk_input_split_report_peripheral_event(2,1,273,1,true));
    full=1;
    assert(!zmk_input_split_peripheral_disconnected(2));
    assert(!zmk_input_split_report_peripheral_event(2,1,273,1,true));
    full=0;
    retry_releases(NULL);
    assert(n==3 && last==1);
    assert(!zmk_input_split_peripheral_disconnected(2) && n==4 && !last);
    assert(zmk_input_split_peripheral_disconnected(99)==-ENODEV);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "split.c"
    path.write_text(source)
    exe = path.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Werror", str(path), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

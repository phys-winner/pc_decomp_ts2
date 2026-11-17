/* Pure topology selection; returning 0 leaves the output untouched. */
enum { RT_CORES_AUTO, RT_CORES_BIG, RT_CORES_NEXT, RT_CORES_ANY };

static int d3d_rt_select_cores(const long *capacity, int count, int policy, cpu_set_t *out)
{
    if (!out || count <= 0 || count > CPU_SETSIZE || policy < RT_CORES_AUTO || policy > RT_CORES_ANY) return 0;
    if (policy == RT_CORES_ANY) {
        CPU_ZERO(out);
        for (int i = 0; i < count; ++i) CPU_SET(i, out);
        return 1;
    }
    if (!capacity) return 0;
    long best = 0, next = 0, third = 0;
    for (int i = 0; i < count; ++i) {
        long value = capacity[i];
        if (value <= 0) return 0;
        if (value > best) { third = next; next = best; best = value; }
        else if (value < best && value > next) { third = next; next = value; }
        else if (value < next && value > third) third = value;
    }
    if (!next) return 0;
    int biggest = 0;
    for (int i = 0; i < count; ++i) biggest += capacity[i] == best;
    long chosen = policy == RT_CORES_BIG ? best : policy == RT_CORES_NEXT ? next
        : third || biggest == 1 ? next : best;
    CPU_ZERO(out);
    for (int i = 0; i < count; ++i) if (capacity[i] == chosen) CPU_SET(i, out);
    return 1;
}

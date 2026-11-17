/* SPDX-License-Identifier: Apache-2.0 */
const char *__asan_default_options(void)
{
    return "detect_leaks=0";
}

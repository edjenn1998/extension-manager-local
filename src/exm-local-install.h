/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
#include "local/exm-manager.h"

void exm_local_install_choose (GtkWindow *parent, ExmManager *manager);
void exm_local_install_open_file (GtkWindow *parent, ExmManager *manager, GFile *file);

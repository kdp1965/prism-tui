/************************************************************************************
 * tui/TuiMain.cxx
 *
 * Entry point for the 'tui' console command: builds the CPrism source
 * and the CTui screen, runs the (single threaded) UI loop until the
 * user exits, then hands the terminal back to the plain CLI.
 ************************************************************************************/

#include <stdio.h>

#include "Tui.h"
#include "Prism.h"

extern "C" {
#include "../prism_tui.h"
}

extern "C" void tui_run(void)
{
  CPrism *pPrism = new CPrism;
  CTui   *pTui   = new CTui;

  if (pPrism == NULL || pTui == NULL)
  {
    printf("tui: out of memory\n");
    delete pTui;
    delete pPrism;
    return;
  }

  pTui->m_pPrompt = "prism> ";
  pTui->AttachTuiSource(pPrism);

  // Legacy command printf output -> TUI command window
  pPrism->InstallStdoutHook();

  pTui->RunThread();

  pPrism->RemoveStdoutHook();

  delete pTui;
  delete pPrism;

  printf("\n(tui closed)\n");
}

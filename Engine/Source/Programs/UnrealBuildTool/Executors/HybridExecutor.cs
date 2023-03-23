// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace UnrealBuildTool
{
	/// <summary>
	/// Executor which distributes large sets of parallel actions through a remote executor, and any leaf serial actions through a local executor. Currently uses
	/// XGE for remote actions and ParallelExecutor for local actions.
	/// </summary>
	class HybridExecutor : ActionExecutor
	{
		/// <summary>
		/// Maximum number of actions to execute locally.
		/// </summary>
		[XmlConfigFile(Category = "HybridExecutor")]
		int MaxLocalActions;

		/// <summary>
		/// Executor to use for remote actions
		/// </summary>
		ActionExecutor RemoteExecutor;

		/// <summary>
		/// Executor to use for local actions
		/// </summary>
		ActionExecutor LocalExecutor;

		/// <summary>
		/// Constructor
		/// </summary>
		public HybridExecutor(int InMaxLocalActions)
		{
			MaxLocalActions = InMaxLocalActions;
			this.LocalExecutor = new ParallelExecutor(MaxLocalActions);
			this.RemoteExecutor = new XGE();

			XmlConfig.ApplyTo(this);

			if(MaxLocalActions == 0)
			{
				MaxLocalActions = Utils.GetPhysicalProcessorCount();
			}
		}

		/// <summary>
		/// Tests whether this executor can be used
		/// </summary>
		/// <returns>True if the executor may be used</returns>
		public static bool IsAvailable()
		{
			return XGE.IsAvailable() && ParallelExecutor.IsAvailable();
		}

		/// <summary>
		/// Name of this executor for diagnostic output
		/// </summary>
		public override string Name
		{
			get { return String.Format("hybrid ({0}+{1})", LocalExecutor.Name, RemoteExecutor.Name); }
		}

		/// <summary>
		/// Execute the given actions
		/// </summary>
		/// <param name="ActionsToExecute">Actions to be executed</param>
		/// <returns>True if the build succeeded, false otherwise</returns>
		public override bool ExecuteActions(List<LinkedAction> ActionsToExecute)
		{
			// Find the number of dependants for each action
			Dictionary<LinkedAction, int> ActionToNumDependents = ActionsToExecute.ToDictionary(x => x, x => 0);
			foreach(LinkedAction Action in ActionsToExecute)
			{
				foreach(LinkedAction PrerequisiteAction in Action.PrerequisiteActions)
				{
					ActionToNumDependents[PrerequisiteAction]++;
				}
			}

			// Build up a set of leaf actions in several iterations, ensuring that the number of leaf actions in each 
			HashSet<LinkedAction> LeafActions = new HashSet<LinkedAction>();
			for(;;)
			{
				// Find all the leaf actions in the graph
				List<LinkedAction> NewLeafActions = new List<LinkedAction>();
				foreach(LinkedAction Action in ActionsToExecute)
				{
					if(ActionToNumDependents[Action] == 0 && !LeafActions.Contains(Action))
					{
						NewLeafActions.Add(Action);
					}
				}

				// Exit once we can't prune any more layers from the tree
				if(NewLeafActions.Count == 0 || NewLeafActions.Count >= MaxLocalActions)
				{
					break;
				}

				// Add these actions to the set of leaf actions
				LeafActions.UnionWith(NewLeafActions);

				// Decrement the dependent counts for any of their prerequisites, so we can try and remove those from the tree in another iteration
				foreach(LinkedAction NewLeafAction in NewLeafActions)
				{
					foreach(LinkedAction PrerequisiteAction in NewLeafAction.PrerequisiteActions)
					{
						ActionToNumDependents[PrerequisiteAction]--;
					}
				}
			}

			// Split the list of actions into those which should be executed locally and remotely
			List<LinkedAction> LocalActionsToExecute = new List<LinkedAction>(LeafActions.Count);
			List<LinkedAction> RemoteActionsToExecute = new List<LinkedAction>(ActionsToExecute.Count - LeafActions.Count);
			foreach(LinkedAction ActionToExecute in ActionsToExecute)
			{
				if(LeafActions.Contains(ActionToExecute))
				{
					LocalActionsToExecute.Add(ActionToExecute);
				}
				else
				{
					RemoteActionsToExecute.Add(ActionToExecute);
				}
			}

			// Execute the remote actions
			if(RemoteActionsToExecute.Count > 0)
			{
				if (!RemoteExecutor.ExecuteActions(RemoteActionsToExecute))
				{
					return false;
				}
			}

			// Pass all the local actions through to the parallel executor
			if(LocalActionsToExecute.Count > 0)
			{
				if(!LocalExecutor.ExecuteActions(LocalActionsToExecute))
				{
					return false;
				}
			}

			return true;
		}
	}
}
